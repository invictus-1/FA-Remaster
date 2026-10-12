// Decals: pictures laid on the ground - a map's painted details, the scorch mark an explosion leaves.
//
// A decal is not a mesh. After the ground is drawn and before anything stands on it, each decal's box
// is drawn over the picture; for every pixel in it the ground's place in the world is worked out from
// the picture's depth, and where that place lies inside the decal's footprint the decal's texture is
// blended over the ground, lit as the ground under it. So a decal follows the ground exactly, at
// whatever level of detail the ground was drawn, and never climbs onto a unit.
//
// Only albedo decals so far (a picture over the ground's colour). The map editor's other kinds -
// above all normal decals, which shape how the ground takes the light - are still to come.
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "textures.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

class Decals {
 public:
  Decals(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);

  // A decal: `texture` is a path, or a script's short name ("Scorch_generic_002_albedo": looked for
  // in /env/common/decals and /env/common/splats). `centre` is on the ground; `sizeX`, `sizeZ` its
  // whole extent in world units; `heading` turns it about the upright axis (radians).
  // `cutoff`: farther from the camera than this it is not drawn (0: always).
  // `born`, `seconds`: when it was made and how long it stays (seconds <= 0: for good); it fades
  // out over its last five seconds. Returns false when the texture is not there.
  bool Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // A glow on the ground that adds its light for `seconds`, coloured by `ramp` along that time:
  // the light an explosion throws on its surroundings (the original's "light particle", there a
  // flat sheet drawn over everything; here it lies on the ground). `texture` and `ramp` are paths;
  // `size` is half its edge.
  bool AddLight(const std::string& texture, const std::string& ramp, Vec3 centre, float size, double born, float seconds);

  // Draws the decals into the current pass, which must be the scene's colour alone (no depth
  // target); `sceneDepth` is the scene's depth with the ground in it.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now);

  float glow = 2.5f;  // the modern look's brightness of a light on the ground (as Particles::glow)
  int drawn = 0;  // in the last Draw
  int Count() const { return static_cast<int>(decals_.size()); }

 private:
  struct Decal {
    gfx::Texture texture, ramp;  // ramp: only a light has one
    Vec3 centre;
    float halfX, halfZ, heading, cutoff;
    double born;
    float seconds;
  };
  struct Instance {  // as the shader reads it
    float centre[3], halfHeight;
    float halfX, halfZ, cosHeading, sinHeading;
    float alpha, age, light, glow;  // age: 0..1 of a light's time; light: 1 = adds its light
  };
  gfx::Texture Resolve(const std::string& texture);

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, lights_;
  gfx::Buffer cube_;
  std::vector<Decal> decals_;
  struct Visible {
    uint32_t texture, ramp;
    Instance instance;
  };
  std::vector<Visible> visible_;
  std::vector<Instance> batch_;
};

}  // namespace render
