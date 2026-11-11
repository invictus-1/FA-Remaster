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
// glow (a light to all sides: its steady part, [nightlight]); a beam's way and the cosine of its edge (below -1: a light to all sides); its shadow map's
// number (-1 none; -2 none, and it rides on the model standing at y, z, w), then the lamps' maps'
// matrices, then the larger lights' ground tables ([lightshade], below: a light to all sides that has one says in its
// third row where it begins, x, and how strongly it counts, y; a table's first row says where its middle lies, x and
// z, how wide its squares are and how many it has each way).
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

struct MapData;

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
  // [restart] (World::RestartGame, the interface's request 45 R1) A new game on the same map, whose clock begins again near
  // 0: the flashes' lights on the ground let go of, and which beams had a shadow map.
  void Forget() {
    flashes_.clear();
    shadowKeys_.clear();
    shades_.clear();  // [lightshade]
  }
  // A light for this frame. `color` is linear, as bright as the surface it lights fully would be
  // (1, 1, 1: about as the sun lights it).
  // `origin` (a unit's lamp): where the model it rides on stands (its origin), which casts no
  // ray-traced shadow from it (World::rayLightShadows; a lamp set into a hull would be shadowed by
  // the hull).
  // [nightlight] `air`: its steady part, as a share of `color` (1 all of it; a build arc's light its mean over its colour
  // now, not its quick flicker: BuildEffects::AddLights): what the light in the air (air.h) shows, and what the rays of
  // the small lights' shadows choose it by (lighting.h, SmallLightShare). Written in the light's second row's w.
  void Add(Vec3 position, float radius, Vec3 color, const Vec3* origin = nullptr, float air = 1);
  // A beam: light within `coneDegrees` (the whole opening angle) about `direction`, full to seven
  // tenths of the way out from its middle and fading to nothing at its edge, reaching `radius`
  // along the beam; and `glow` of it to all sides within a fifth of the radius, so that the lamp's
  // own surroundings are not left dark.
  // `carrier`: the middle of the bounding sphere of what the lamp rides on, which casts no shadow
  // in its beam (or a lamp set into a hull would be shadowed by the hull); `origin`, as for Add.
  // [nightlight] `key`: the lamp's own number from frame to frame (0: none), by which it keeps its shadow map (steady).
  void AddBeam(Vec3 position, float radius, Vec3 color, Vec3 direction, float coneDegrees, float glow, const Vec3* carrier = nullptr,
               const Vec3* origin = nullptr, uint64_t key = 0);
  // A light that lives `seconds` from `born` and takes its colour and strength along that time from
  // a ramp texture (the scripts' "light particle": `size` is what the original draws its glow with;
  // the light reaches a few times as far). `from`: what threw it.
  void Flash(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from = {});
  // (volume explosions, volume_blasts.h) told of every explosion's flash as Flash is given it, while set: big ones are
  // blasts (a structure begun, a unit being built, a warp-in, a reclaim throw their flashes too, and are none)
  std::function<void(const std::string& ramp, Vec3 position, float size, double born, float seconds, const FlashFrom& from)> onFlash;
  // [warpin] Whether there is such a ramp texture to read (the game's, or one of the mounted remasters').
  bool HasRamp(const std::string& ramp) const;
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
  // [calmflash] (Chris's 11-08 test, item 12: the picture dimming and brightening at night) the farthest a flash that is
  // no explosion's reaches (a warp-in's, a factory's, a structure begun: the game draws these as a glow of `size`, and
  // a warp-in's is 35 - three times that lit everything within 105 of a commander, the whole picture lifting into a grey
  // wash for two seconds and falling back). Explosions keep their reach (a nuke lights the land round it).
  float calmFlashReach = 24.0f;
  float night = 1;            // what flashes are multiplied by this frame (the night's dimming of the game's effects)
  // [nightlight] (World::steadyLamps) A beam that had a shadow map the frame before keeps it while no other is clearly nearer
  // to what the camera looks at (a quarter nearer), so that two lamps about as near do not take it in turns, the one
  // without its shadow map shadowed by rays or not at all.
  bool steady = false;
  // [mergeflux] lights merged in crowded squares (Build) keep the light of those they stand for in all, not all their
  // colours at their middle (false: as before, viewer --merge-bright)
  bool mergeFlux = true;
  int shadowSwaps = 0;  // (MOHO64_DEBUG_NIGHTLIGHT) beams given a shadow map in the last PlaceShadows that had none the frame before
  // [lightshade] (the review of 2026-11-09, light 11: "small lights without a lamp shadow map shine through hills") The
  // ground shadows the larger lights to all sides: an explosion's flash or a big glow behind a ridge lit the ground, the
  // units, the smoke and the mist on the ridge's near side as if the ridge were not there, and from the game's camera a
  // nuke's flash lit the far slope of a hill as brightly as the near one. Each such light (a reach of `shadeFrom` world
  // units or more; in full from `shadeFull`, so that a light whose reach wavers about the line does not pop) gets a
  // table of squares laid over the ground it reaches (32, 64 or 128 each way by its reach: a square 4 world units wide
  // or less up to a reach of 256, a nuke's flash of 900 squares of 14), worked out here from the map's heights (Build;
  // ShadeTable) and read by every shader that reads the lights (light_shade.h's LightShade: the lit surfaces with and
  // without the rays, what the rays meet, the air, smoke, dust, spray, rain). A square holds two heights: below the
  // first nothing of the light is seen from there, above the second all of it; between them a share (the light is not
  // a point: it stands `shadeSize` of its reach tall over its place, `shadeTall` world units at most, so a shadow's
  // edge is the softer the farther behind the ridge, and ground that merely rolls is not shadowed by its own bumps).
  // The ground within `shadeBall` of the reach round the light casts no shadow (as the rays' own: World::rayLightSize;
  // the crater a flash sits in; `shadeBallMost` world units at most: a nuke's flash reaches 320 and more, and a fifth of
  // that would take every hill near it out), and the ground under a square itself does not count for it (a lit slope
  // never shadows itself: the shadow begins a square behind the crest). What stands on the ground (hulls, buildings,
  // trees) is not in the tables: only the rays find those (lighting.h's SmallLightsTraced), on the lit surfaces alone.
  // At most kShadeMost lights a frame, the ones that throw the most light, in kShadeRowsMost rows of the buffer at
  // most (the lights that throw least take coarser tables where they would be more). A table is kept while its light
  // stands where it stood (a flash does), or has moved less than an eighth of a square (a nuke's rising glow: made
  // anew every few frames; the table says where its own middle lies, so its shadows stand still meanwhile), so most
  // frames make none.
  // `ground`: the map's heights (none: no tables); GroundChanged when they change within that rectangle (the tables
  // kept of lights that reach it are made anew). Off: the buffer as before, to the byte.
  bool shade = true;
  float shadeFrom = 10, shadeFull = 14;
  float shadeSize = 0.1f, shadeTall = 12, shadeBall = 0.2f, shadeBallMost = 8;
  const MapData* ground = nullptr;
  void GroundChanged(float x0, float z0, float x1, float z1);
  static constexpr int kShadeMost = 64;         // (a fight's shells and deaths: some fifty flashes of a reach of 10 to 24 at once)
  static constexpr int kShadeRowsMost = 49152;  // (768 KB of the buffer: six of the largest tables, or all kShadeMost of the smallest and one of the largest)
  double shadeMs = 0;    // the processor's time for the tables made, in all (MOHO64_DEBUG_LIGHTSHADE says each one made)
  int shaded = 0;        // lights with a table in the last Build
  int shadeMade = 0;     // tables worked out in it (the others kept from frames before)
  int shadeLeftOut = 0;  // larger lights left without one (more than kShadeMost)
  int shadedMost = 0;    // (the most in one Build so far; and tables made in all, and the most lights left without)
  int shadeMadeAll = 0, shadeLeftOutMost = 0;
  int leftOut = 0;      // lights left out of crowded squares in the last Build (each square's count over kMostInSquare, summed)
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
    uint64_t key = 0;     // [nightlight] a beam's own number (AddBeam), 0 none
    float air = 1;        // [nightlight] a light to all sides: its steady part (Add)
    Vec3 origin;          // where the model it rides on stands (hasOrigin): no ray-traced shadow from that
    bool hasOrigin = false;
  };
  struct FlashLight {
    const std::vector<Vec3>* ramp;  // colour x alpha along its time
    Vec3 position;
    float size;
    double born;
    float seconds;
    bool explosion;  // ([calmflash] else its reach is held to calmFlashReach)
  };
  const std::vector<Vec3>* Ramp(const std::string& path);
  // [lightshade] a light's ground table: n x n squares, two heights each (nothing seen below, all above)
  void ShadeTable(Vec3 at, float radius, int n, float* lines);
  struct Shade {
    Vec3 position;
    float radius = 0;
    int cells = 0;      // (squares each way)
    uint64_t used = 0;  // (the last Build that read it)
    std::vector<float> lines;
  };
  std::vector<Shade> shades_;  // (tables kept: a light that stands where it stood keeps its own)
  std::vector<float> shadeSlopes_;  // (ShadeTable's working room)
  const uint16_t* shadeHeights_ = nullptr;  // (the heights the tables kept were made from: another map's are let go of)
  uint64_t shadeBuild_ = 0;

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  gfx::Buffer buffer_;
  gfx::Texture shadowMap_;
  View shadowViews_[kLampShadows];
  Mat4 shadowMatrix_[kLampShadows];  // world -> a lamp's map (x, y over w: where in the map; z over w: depth)
  int shadowCount_ = 0;
  std::vector<uint64_t> shadowKeys_;  // [nightlight] (steady) the beams that had a shadow map the frame before, by key
  std::vector<Light> lights_;
  std::vector<FlashLight> flashes_;
  std::unordered_map<std::string, std::vector<Vec3>> ramps_;
  std::vector<float> rows_;
  std::vector<uint32_t> cellCount_, cellFirst_, indices_;
  int cellStep_ = -1;  // (unitsfx) the grid's squares' size, as a step of its ladder (lights.cpp, Build)
  double now_ = 0;
};

}  // namespace render
