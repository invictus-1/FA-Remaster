// Life in the world (World::wildlife; the modern look): flocks of birds over the map's woods and gulls over its
// shores, and fireflies at dusk and by night along the woods' edges and over grass. Purely for the eye: nothing of it
// goes back to the game, it has random numbers of its own, and it is never picked, selected or given an icon.
//
// Birds. Each flock has a home: a wood (where the map's trees stand thickest; World::Open hands them over) or a
// stretch of water off a shore (gulls). A flock rests in its wood's crowns (hidden there), takes off now and then
// and wheels over its home, and lands again; at dusk the land birds go to roost (the starlings last, wheeling in the
// last light), the gulls a little later; by night they rest. Fighting near a flock (a shell landing, a hit, a shot
// fired, a beam, an explosion's flash: Hear, Listen) scatters it: each bird bursts off when the noise reaches it,
// away from it and up, then the flock makes for a place away from the fight and wheels there until it has been
// quiet a while, comes home and settles.
// The flocks' states are kept on the processor (a few numbers each), the birds themselves on the card: a compute
// pass moves them, a group of 32 threads a flock (boids: each bird keeps apart from those nearest it, flies with
// them and keeps to them, and is drawn toward where its flock is going, above the ground and the trees), and a draw
// makes each a few triangles: a body, a tail and two wings in two parts that flap or glide. Small; starlings and
// crows black with a sheen that catches the sun, pigeons grey, gulls grey above and white below (all dark against
// the sky, which sees their shaded undersides); fewer as the camera rises and none from the strategic zoom.
//
// Fireflies: no state at all. A square of cells round what the camera looks at, each cell a few places where one
// may be, kept or not by how much of a wood's edge and of grass is there; each drifts slowly a little over the
// ground and glows now and then for half a second, yellow-green, seen through the air (and through the light in
// the air, World::volumetricFog) as anything else is.
//
// Off, nothing of it is made or done.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "particles.h"
#include "view.h"

namespace render {

struct MapData;

class Wildlife {
 public:
  explicit Wildlife(gfx::Device* device) : device_(device) {}
  ~Wildlife();

  // The map (its heights and water) and where its trees stand: x, the ground's height, z, and the tree's height
  // (world units) each. The flocks are given their homes here: as many as the woods and shores make room for.
  void SetMap(const MapData* map, const std::vector<Vec4>& trees);

  // Noise of fighting at a place, taken at the next Update: `loudness` 1 a shell landing (heard 40 world units
  // round), 0.5 a shot fired, 2 or more a large blast (heard as far as 40 x its root).
  void Hear(Vec3 at, float loudness);
  // What a frame of the game says of fighting (World::AddFrame): shots first seen (a weapon firing) and gone (they
  // met something), weapons' beams, and the engine's impacts and area hits where the host's simulation keeps them.
  // Only what the army the picture is for sees (the frame's `seen`): a flock scattering must not give away a fight in
  // the fog of war.
  void Listen(const Frame& frame, const FrameCombat& combat);

  // Each frame it is on, before the scene's pass (it dispatches): the flocks moved on to `time` (seconds, the
  // world's own clock: World::Begin's), and what is drawn this frame chosen. `sunHeight`: the sine of the sun's elevation (the
  // computed sky's SkyLight::body.y, below 0 at night; 1 under the map's own light); `rain`, `snowLying` as World has them;
  // `flashes`: the explosions' flashes (Particles::flashes), those started since the last call heard; `heights`,
  // `heightUnit`: the ground's (Terrain::HeightTexture, HeightUnit); `groundColours`: Terrain::GroundColours (its
  // alpha: how grassy). False when there is nothing to draw.
  bool Update(const View& view, const Camera& camera, float time, float sunHeight, float rain, float snowLying,
              const std::vector<Particles::Flash>& flashes, gfx::Texture heights, float heightUnit, gfx::Texture groundColours);
  // Into the scene's pass: the birds with what is solid (after the models), the fireflies with what glows (after
  // what is see-through), each as the last Update chose. `shadow`: the birds into a pass of the sun's shadow maps
  // (`view` that map's), their depth alone.
  void DrawBirds(const View& view, bool shadow = false);
  void DrawFireflies(const View& view);
  // (from a frame drawn for something else - the minimap - or with the option off: the next Update starts the
  // birds' clock afresh, so they do not leap)
  void Pause() { lastTime_ = -1; }

  // Tuning and numbers (viewer --wildlife-*).
  float birdScale = 1;       // the birds' size, times their own (about twice a real bird's, to read from the game's camera)
  float fireflyStrength = 1; // how bright the fireflies glow
  float fireflyDensity = 1;  // how many there are
  bool verbose = false;      // says where the flocks are and what they do
  int Flocks() const { return static_cast<int>(flocks_.size()); }
  int Birds() const { return birds_; }
  int birdsDrawn = 0, flocksDrawn = 0, firefliesTried = 0;  // the last frame: birds of the flocks drawn; firefly places gone through
  int scares = 0;            // flocks scattered since the map was opened
  int heard = 0;             // noises heard since the map was opened
  // A flock's home and what it does now, for the viewer (verbose): its kind (0 starlings, 1 crows, 2 pigeons, 3 gulls)
  // and its state (0 resting, 1 aloft, 2 fleeing, 3 landing).
  struct FlockInfo {
    Vec3 home;
    int kind, state, birds;
  };
  std::vector<FlockInfo> Nearest(Vec3 at, size_t most) const;

  static constexpr int kBirdsMost = 32;   // birds a flock at most (a group of threads)
  static constexpr int kFlocksMost = 224; // flocks a map at most

 private:
  enum State { Resting = 0, Aloft = 1, Fleeing = 2, Landing = 3 };
  struct Flock {
    Vec3 home;           // the middle of its wood at the height of the crowns (gulls: the water's surface)
    float reach = 30;    // how far round its home it wanders
    float clearance = 4; // how high it keeps over the ground (land birds: over their wood's crowns)
    int kind = 0;        // 0 starlings, 1 crows, 2 pigeons, 3 gulls
    int birds = 0;
    State state = Resting;
    float since = 0;     // when the state began (Update's clock)
    float until = 0;     // when it ends (resting, aloft, landing), or when the next aim is due (fleeing)
    float aimUntil = 0;  // when the next place to make for is chosen (aloft)
    Vec3 aim;            // where it makes for now
    Vec3 refuge;         // fleeing: the place away from the fight it wheels round
    Vec3 scareFrom;      // the last noise that scattered it
    float scareAt = -1e9f, quietSince = -1e9f;
    float settleAfter = 30;  // seconds of quiet after which it comes home
    bool returning = false;  // aloft: on its way home after a scare (then it lands)
    uint32_t random = 1;
  };
  struct Noise {
    Vec3 at;
    float loudness;
  };
  float Random(Flock& f, float lo, float hi);
  void Plan(float now, float sunHeight);
  void Scatter(Flock& f, const Noise& n, float now);
  void PickAim(Flock& f, float now);
  bool Ready();  // the pipelines and the birds' targets, the first time (false: cannot be drawn)
  float GroundAt(float x, float z) const;

  gfx::Device* device_;
  const MapData* map_ = nullptr;
  float mapWidth_ = 0, mapHeight_ = 0, water_ = -1e9f;
  std::vector<Flock> flocks_;
  std::vector<float> perches_;      // per flock and bird: where it sits (x, y, z, 0), kBirdsMost a flock
  std::vector<uint8_t> woods_;      // how thick the woods stand, over the map in squares of kWoodsCell
  int woodsW_ = 0, woodsH_ = 0;
  int birds_ = 0;
  std::vector<Noise> heard_;        // since the last Update
  struct Seen {
    Vec3 at;
    uint32_t stamp = 0;
    bool seen = true;  // (by the army the picture is for, when last in a frame)
  };
  std::unordered_map<uint32_t, Seen> shots_, beams_;  // the frame's shots and beams last seen (Listen)
  uint32_t stamp_ = 0;
  double lastFlash_ = -1e9;         // the newest flash heard (its start, on the particles' clock)
  float lastTime_ = -1;             // Update's clock at the last step (-1: none yet)
  float now_ = 0;
  bool planned_ = false;            // the flocks' first states chosen (at the first Update: by the hour)
  bool fresh_ = true;               // the birds are to be placed afresh at the next step
  bool tried_ = false, made_ = false;
  gfx::Pipeline step_, birdsDraw_, birdShadows_, fireflyDraw_;
  gfx::Texture place_[2], motion_[2], perchMap_, woodsMap_;  // the birds' state (read one set, write the other)
  int cur_ = 0;
  // this frame's draws: their constants (the birds': the flocks and which of them are drawn; the fireflies' square)
  std::vector<float> birdConstants_;
  float fireflyConstants_[16] = {};
  bool drawBirds_ = false, drawFireflies_ = false;
  gfx::Texture heights_, ground_;
  int fireflyCount_ = 0;
};

}  // namespace render
