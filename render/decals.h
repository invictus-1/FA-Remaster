// Decals: pictures laid on the ground - a map's painted details, the scorch mark an explosion leaves.
//
// A decal is not a mesh. After the ground is drawn and before anything stands on it, each decal's box
// is drawn over the picture; for every pixel in it the ground's place in the world is worked out from
// the picture's depth, and where that place lies inside the decal's footprint the decal's texture is
// blended over the ground, lit as the ground under it. So a decal follows the ground exactly, at
// whatever level of detail the ground was drawn, and never climbs onto a unit.
//
// Three kinds: albedo decals (a picture over the ground's colour); normal decals, which tilt how the
// ground takes the light and so sculpt cliffs and erosion into it - here by multiplying the lit
// ground with (the light it would take tilted) / (the light it took); and lights (a glow added).
// The map editor's remaining kinds (glow, water masks) are not drawn.
#pragma once
#include <functional>
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
  // The resource deposits a game has marked on the ground (the script's splats mass_marker and
  // hydrocarbon_marker): where, which, and from how far the mark is drawn (its cutoff). For the
  // icons that take the marks' place from far up (world.cpp).
  struct Deposit {
    Vec3 position;
    bool hydrocarbon = false;
    float cutoff = 0;
  };
  const std::vector<Deposit>& Deposits() const { return deposits_; }
  // Is that texture one of the two marks a game lays on its resource deposits? (They belong to the
  // map: every army sees every deposit from the first moment, as the original shows them.)
  static bool IsDepositMark(const std::string& texture, bool* hydrocarbon = nullptr);
  // Snow and the marks: the host says each frame how much snow lies (0..1) and how hard it snows
  // (0..1). A mark laid on lying snow (a tread's print, a footprint, a scorch) shows ON the snow,
  // and new snowfall covers it in about a minute and a half of heavy fall; a mark that was there
  // before the snow lies under it like the ground.
  float snowLying = 0, snowing = 0;
  // The remastered effects look (World::effectsLook; set by the world each frame, so the map's own decals, laid
  // when it opens, stay the game's): a script's decal is read from the visuals track's own picture of it where it
  // has made one (/remastered, effect_files.h), and lit over the relief beside it where there is one (its
  // "_albedo" named "_normals": a two-channel normal map, x in red, y in green pointing up the picture, OpenGL's).
  bool remastered = false;
  // The picture a script's mark is named by (a path, or a short name looked up in the game's two
  // folders of them); none: not there.
  gfx::Texture Picture(const std::string& texture) { return Resolve(texture, nullptr); }
  Decals(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);

  // A decal: `texture` is a path, or a script's short name ("Scorch_generic_002_albedo": looked for
  // in /env/common/decals and /env/common/splats). `centre` is on the ground; `sizeX`, `sizeZ` its
  // whole extent in world units; `heading` turns it about the upright axis (radians).
  // `cutoff`: farther from the camera than this it is not drawn (0: always).
  // `born`, `seconds`: when it was made and how long it stays (seconds <= 0: for good); it fades
  // out over its last five seconds, or over the second half of its time when that is shorter (a
  // tank's tread marks last a second). Returns false when the texture is not there.
  bool Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The same for a normal decal (its texture: the tilt along its own x in alpha, along its own z in
  // green, how much of it to apply in red).
  bool AddNormal(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The same for a "Water Albedo" decal (the campaign's objective areas): in its own colours, unlit
  // (the original's TDecalsWaterAlbedo: times the map's light multiplier), and where the ground under
  // it lies under water, on the water's surface (drawn by the second Draw, after the water).
  bool AddWater(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The number of the decal the last Add, AddNormal or AddWater laid (from 1), for Remove.
  uint32_t LastAdded() const { return lastId_; }
  // Takes that decal away at once (a script's DecalHandle:Destroy: an objective done, a building's
  // tarmac laid again with a life time when it is killed).
  void Remove(uint32_t id);
  // The ground's height (not the water's) at a place, given by the host: a decal whose place is
  // given far from the ground (the campaign's objective areas are made at a height of 0) is put
  // on the ground there. `waterLevel`: where the water's surface is (the map's; none: very low).
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;
  // A glow on the ground that adds its light for `seconds`, coloured by `ramp` along that time:
  // the light an explosion throws on its surroundings (the original's "light particle", there a
  // flat sheet drawn over everything; here it lies on the ground). `texture` and `ramp` are paths;
  // `size` is half its edge.
  bool AddLight(const std::string& texture, const std::string& ramp, Vec3 centre, float size, double born, float seconds);

  // A mark for this frame only, drawn in its own colours whatever the light (the ring under a
  // selected unit). Forgotten after the next Draw.
  void AddMark(gfx::Texture texture, Vec3 centre, float sizeX, float sizeZ, float heading);
  // The map's water: its depth picture and colour ramp (Terrain::WaterMap, WaterRamp) and the map's
  // size in world units. Pictures lying under water are then tinted as the ground there is.
  void SetWater(gfx::Texture waterMap, gfx::Texture waterRamp, float mapWidth, float mapHeight) {
    waterMap_ = waterMap;
    waterRamp_ = waterRamp;
    mapWidth_ = mapWidth;
    mapHeight_ = mapHeight;
  }
  // Draws the decals into the current pass, which must be the scene's colour alone (no depth
  // target); `sceneDepth` is the scene's depth with the ground in it. `overWater`: the second
  // drawing, after the water: only the "Water Albedo" decals' parts that lie over water.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now, bool overWater = false);
  bool AnyWater() const { return waterDecals_ > 0; }

  float glow = 2.5f;  // the modern look's brightness of a light on the ground (as Particles::glow)
  int drawn = 0;  // in the last Draw
  int Count() const { return static_cast<int>(decals_.size() + marks_.size()); }

 private:
  struct Decal {
    gfx::Texture texture, ramp;  // ramp: only a light has one
    Vec3 centre;
    float halfX, halfZ, heading, cutoff;
    double born;
    float seconds;
    bool normal = false;
    bool mark = false;
    bool water = false;  // a "Water Albedo" decal
    uint32_t id = 0;     // (laid by Add: from 1)
    float snowAtBirth = 0;  // snowFallen_ when it was laid (see `snowLying`)
    gfx::Texture relief;    // a remastered picture's own relief (`remastered`), or none
  };
  struct Instance {  // as the shader reads it
    float centre[3], halfHeight;
    float halfX, halfZ, cosHeading, sinHeading;
    float alpha, age, light, glow;  // age: 0..1 of a light's time; light: 1 = adds its light, 2 = a normal decal;
                                    // glow: a light's brightness, a picture's 1 when it has its own relief
  };
  // The picture a decal is named by (none: not there), and with `remastered` its relief where there is one.
  gfx::Texture Resolve(const std::string& texture, gfx::Texture* relief);
  float snowFallen_ = 2;    // all the snow that ever fell, in coverings of a mark (it only grows)
  double snowClock_ = -1;   // when it was last moved on
  bool settled_ = false;    // false until the first frame: marks laid before it are the map's own, old
  uint32_t nextId_ = 1, lastId_ = 0;
  int waterDecals_ = 0;     // "Water Albedo" decals there now

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, lights_, normals_;
  gfx::Buffer cube_;
  gfx::Texture waterMap_, waterRamp_;
  float mapWidth_ = 0, mapHeight_ = 0;
  std::vector<Decal> decals_;
  std::vector<Deposit> deposits_;
  std::vector<Decal> marks_;  // this frame's
  struct Visible {
    uint32_t texture, ramp, relief;
    int kind;  // drawn in this order: 0 pictures, 1 normal decals, 2 lights, 3 marks
    Instance instance;
  };
  std::vector<Visible> visible_;
  std::vector<Instance> batch_;
};

}  // namespace render
