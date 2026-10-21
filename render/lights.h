// Small lights: the flash of an explosion, later muzzle flashes, shots, lamps on units and buildings.
// Each is a point that lights what is within its radius, fading to nothing at the radius.
//
// So that thousands stay cheap, the lights are sorted every frame into a grid of squares laid over
// the ground (in this game everything of interest is near the ground, so a flat grid does what
// other renderers need a grid in the picture's depth for): each square lists the lights that reach
// it, and a pixel looks only at the lights of the square its place on the ground is in. The grid
// and the lights go to the shaders in one buffer (lighting.h reads it). A pixel looks at 48 at most;
// where more reach a square (a big unit's salvos landing), the lights crowding it are merged first
// (Build), so that the light does not step at the squares' edges.
//
// The beams of the lamps nearest to what the camera looks at also cast shadows: each of those has a
// small shadow map of its own, a picture of the depth of units and scenery as the lamp sees them.
// (The ground is not in it: a lamp low over the ground would shadow all of it with itself.)
// With World::rayLightShadows every other light is shadowed by rays instead (lighting.h,
// SmallLightsTraced).
//
// The buffer (Build): a row of four numbers per square of the grid (where its list begins, how
// long it is), the lists, then four rows per light: its place and radius; its colour and a beam's
// glow; a beam's way and the cosine of its edge (below -1: a light to all sides); its shadow map's
// number (-1 none; -2 none, and it rides on the model standing at y, z, w), then the lamps' maps'
// matrices.
//
// Only the modern look uses them. The original game has no such lights: there the "light particle"
// of an explosion is a glow laid on the ground (decals.h), which the original look keeps.
#pragma once
#include <cstdint>
#include <functional>
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

// What threw a flash, as the effects know it (Effects::Apply), for those told of it (Lights::onFlash: the volume
// explosions, volume_blasts.h).
struct FlashFrom {
  bool explosion = true;  // an explosion's (Effects::Explosion), not a script's light on a living unit or a prop
  std::string faction;    // whose: "uef", "aeon", "cybran", "seraphim" ("": not known)
  float dyingWidth = 0;   // a dying unit's: half its model's width (world units, the larger of across and along); 0: none
};

class Lights {
 public:
  Lights(gfx::Device* device, const moho::Vfs* vfs);

  // Empties this frame's list; `now` is the effects' clock, seconds.
  void Begin(double now);
  // A light for this frame. `color` is linear, as bright as the surface it lights fully would be
  // (1, 1, 1: about as the sun lights it).
  // `origin` (a unit's lamp): where the model it rides on stands (its origin), which casts no
  // ray-traced shadow from it (World::rayLightShadows; a lamp set into a hull would be shadowed by
  // the hull).
  void Add(Vec3 position, float radius, Vec3 color, const Vec3* origin = nullptr);
  // A beam: light within `coneDegrees` (the whole opening angle) about `direction`, full to seven
  // tenths of the way out from its middle and fading to nothing at its edge, reaching `radius`
  // along the beam; and `glow` of it to all sides within a fifth of the radius, so that the lamp's
  // own surroundings are not left dark.
  // `carrier`: the middle of the bounding sphere of what the lamp rides on, which casts no shadow
  // in its beam (or a lamp set into a hull would be shadowed by the hull); `origin`, as for Add.
  void AddBeam(Vec3 position, float radius, Vec3 color, Vec3 direction, float coneDegrees, float glow, const Vec3* carrier = nullptr,
               const Vec3* origin = nullptr);
  // A light that lives `seconds` from `born` and takes its colour and strength along that time from
  // a ramp texture (the scripts' "light particle": `size` is what the original draws its glow with;
  // the light reaches a few times as far). `from`: what threw it.
  void Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from = {});
  // (volume explosions, volume_blasts.h) told of every explosion's flash as Flash is given it, while set: big ones are
  // blasts (a structure begun, a unit being built, a warp-in, a reclaim throw their flashes too, and are none)
  std::function<void(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from)> onFlash;
  // The colour a thing glows in whose particles take theirs from that ramp texture: the ramp's
  // strongest place (linear light), for the light a shot in flight throws.
  Vec3 RampGlow(const std::string& ramp);
  // Sorts this frame's lights (those added, and the flashes alive now) into the grid, hands them to
  // the shaders and writes where the grid is into `view`. Call once a frame, before the scene's passes.
  void Build(View* view);
  // Binds the lights' buffer for the pass just begun (every scene pass whose shaders light surfaces).
  void Bind();
  // The lamps' shadows. PlaceShadows (after the frame's lamps are added, before Build) picks the
  // beams nearest to what the camera looks at, at most kLampShadows, and returns how many; the host
  // then draws what casts shadows into each one's pass with its view. `view` is the scene's (levels
  // of detail are measured from the camera in these passes too).
  static constexpr int kLampShadows = 8;
  static constexpr int kLampShadowSize = 512;  // lighting.h's shader steps by 1 / this
  int PlaceShadows(const Camera& camera, const View& view);
  const View& ShadowView(int i) const { return shadowViews_[i]; }
  gfx::PassDesc ShadowPass(int i) const;
  gfx::Texture ShadowMap() const { return shadowMap_; }

  float strength = 3.0f;      // how bright a flash of full ramp colour is
  float reach = 3.0f;         // a flash's radius, in sizes of its glow
  float night = 1;            // what flashes are multiplied by this frame (the night's dimming of the game's effects)
  int count = 0;              // lights in the last Build
  // [unitfx] lights merged into others in the last Build, where squares of the grid were too crowded (Build: a hundred
  // shots' glows and hits' flashes where a big unit's salvos land); 0 in a frame with no square over
  int merged = 0;
  int mergedMost = 0;  // (the most in one Build so far)

 private:
  struct Light {
    Vec3 position;
    float radius;
    Vec3 color;
    float glow = 1;       // a beam: the share that still shines to all sides close to the lamp
    Vec3 direction;       // a beam: where it points
    float cosEdge = -2;   // a beam: the cosine of half its opening angle; below -1: it shines to all sides
    Vec3 carrier;         // a beam: the middle of what it rides on (hasCarrier), which does not shadow it
    bool hasCarrier = false;
    int shadow = -1;      // a beam: which shadow map is its own this frame, or -1
    Vec3 origin;          // where the model it rides on stands (hasOrigin): no ray-traced shadow from that
    bool hasOrigin = false;
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
  gfx::Texture shadowMap_;
  View shadowViews_[kLampShadows];
  Mat4 shadowMatrix_[kLampShadows];  // world -> a lamp's map (x, y over w: where in the map; z over w: depth)
  int shadowCount_ = 0;
  std::vector<Light> lights_;
  std::vector<FlashLight> flashes_;
  std::unordered_map<std::string, std::vector<Vec3>> ramps_;
  std::vector<float> rows_;
  std::vector<uint32_t> cellCount_, cellFirst_, indices_;
  int cellStep_ = -1;  // (unitsfx) the grid's squares' size, as a step of its ladder (lights.cpp, Build)
  double now_ = 0;
};

}  // namespace render
