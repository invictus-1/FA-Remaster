// Dust, spray and contrails of what moves (World::movementDust; the modern look; Chris's list of 17 October, the
// renderer's item 16): to be seen only, nothing of it goes back to the game.
//
//  - Vehicles raise dust where the ground is dry: puffs kicked up behind their treads or wheels as they go, more the
//    faster they go and the drier and looser the ground (dry sand most, then dry earth, a little on dry grass and
//    gravel; none on green grass, rock, mud or snow, and none on wet ground or in rain). Low and soft, coloured as the
//    ground there (Terrain::GroundColours), lit as the blasts' dust is (and as smoke is, with lit smoke on: the small
//    lights and the smoke's own shadow), drifting with the wind, thinning out in a few seconds. Walkers scuff up less.
//  - Hover units throw spray over water: a ring of it round their skirt, and a trail of it behind fast ones; over land
//    a faint ring of dust. (Their wakes on the water stay as they are: wakes.h; their ripples: ripples.h.)
//  - High aircraft draw contrails: thin ribbons from their engines, only high over the ground (fighters and bombers,
//    not gunships or transports), widening and fading over seconds, longer and thicker in cold, damp air. Grey-white
//    and see-through, never bright: a shot glows and flies; a contrail is pale and stays where it was made.
// Readable: the dust is drawn under the units (over the scenery, then the units over it), low and thin; spray and
// contrails are thin; all counts are capped.
//
// Which ground is dry and loose (DustAt): each of the map's layers judged by what it is (LayerDust: its name, or
// remap.json's "ground" for it where the visuals track gives one), laid over each other by the map's masks as the
// ground's shader lays the layers, on the processor (a grid of two world units a cell, made the first frame the option
// is on); times what the weather has done (Begin: wet ground, rain, lying snow). That judgement is one small function
// (LayerDust), to be switched to the ground kinds of the tracks' work (Terrain::GroundKinds) once both are in.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bp_reader.h"
#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct MapData;
struct Model;

class MovementDust {
 public:
  explicit MovementDust(gfx::Device* device) : device_(device) {}
  ~MovementDust();

  // The map the units move on (World::Open): its layers and masks say where the ground raises dust (worked out the first
  // frame the option is on). `mapPath` and `readFile` (the game's files): remap.json's "ground" for a layer.
  void SetMap(const MapData* map, const std::string& mapPath, std::function<std::optional<std::string>(const std::string&)> readFile);
  // A unit's blueprint by its id ("uel0201"), read once for each kind of unit (World sets it).
  std::function<bool(const std::string&, BpValue*)> readBlueprint;
  // The map under them (World::Open): the ground's height (not the water's), and the water's level (none: very low).
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;

  // How dusty a layer of the ground is when dry, 0 (none: green grass, rock, mud, snow, wet sand) .. 1 (dry sand), by
  // what it is: `given` where the map's remastered ground names it (remap.json "ground", the kinds the tracks read:
  // sand, dirt or soil, gravel, hard, grass, mud, snow, or two with a plus), else by the words of its texture's name
  // ("/maps/x/env/layers/sand_dry_albedo.dds"); a layer its name calls wet raises next to none either way.
  static float LayerDust(const std::string& texture, const std::string& given);
  // How dusty the dry ground is at a place (0..1), by the map's layers (not the weather).
  float DustAt(float x, float z) const;

  // What the weather has done and how the air is (World::Begin, with a time of day; else all dry and mild).
  struct Weather {
    float wet = 0, rain = 0, snow = 0, snowLying = 0;  // as World's
    float clouds = 0.3f, fog = 0;                       // how much of the sky clouds cover; fog
    float daylight = 1;                                 // 0 night .. 1 full day (SkyLight::daylight)
    bool cold = false, dry = false;                     // the map's climate
  };

  // --- a frame ---
  // At the frame's start (World::Begin): the game's clock (seconds); `on` the option this frame (the modern look, a map,
  // not a picture for something else); the weather; where the eye is (nothing far off raises dust).
  void Begin(double clock, bool on, const Weather& weather, Vec3 eye);
  // A unit drawn this frame, alive, built and seen: its entity id, blueprint id, model, place and turn. Raises what its
  // way of moving and the ground under it raise, by how it moved since the last frame. Nothing while off.
  void AddUnit(uint32_t id, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation);
  // Before the scene's pass (it uploads; its shaders made the first time): drops what is over, lists what the view sees.
  void Prepare(const View& view);
  bool Under() const { return drawUnder && groundCount_ > 0; }  // (dust to draw under the units this frame)
  bool Above() const { return sprayCount_ > 0 || segments_ > 0 || (!drawUnder && groundCount_ > 0); }  // (spray and contrails, after the water)
  // The dust, into the scene's colour alone (it tests the scene's depth itself): after the scenery, before the units.
  // `heights`, `heightUnit`: Terrain::HeightTexture, HeightUnit; `groundColours`: Terrain::GroundColours (or none);
  // `lit`: lit smoke is on (the small lights bound, and the smoke's own shadow read where it is drawn).
  void DrawUnder(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                 float mapWidth, float mapHeight, bool lit);
  // The spray and the contrails, the same way: after the water and the sky, before the game's own particles (and the
  // dust too, when it is not drawn under the units).
  void DrawAbove(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                 float mapWidth, float mapHeight, bool lit);
  void Clear();  // (a map opened anew)

  // The wind near the ground (world units a second: x, z): the dust drifts with it; contrails high up drift with
  // `windAloft`. Along the way the trees sway and the clouds drift (mesh_renderer.cpp, sky.cpp); a map has none of its
  // own. A host may set either.
  float wind[2] = {0.915f * 0.6f, 0.403f * 0.6f};
  float windAloft[2] = {0.915f * 2.0f, 0.403f * 2.0f};
  // The dust drawn under the units (over the scenery: the scene's models drawn in two parts, as for range rings), so
  // that it never hides a unit; false: over everything with the spray, as the blasts' dust is (viewer
  // --movement-dust-over: to compare).
  bool drawUnder = true;
  // How much of each there is (1 as tuned): the dust, the spray, the contrails.
  float dustStrength = 1, sprayStrength = 1, contrailStrength = 1;
  // From how high over the ground or water (world units) aircraft draw contrails: fighters and bombers fly at 14 to 20,
  // gunships and transports at 8 to 10.
  float contrailFrom = 12;
  // The most of each kept (the oldest let go first), and how far from the eye a unit still raises dust or spray.
  static constexpr int kMostPuffs = 3000, kMostTrails = 48;
  float farthest = 480;

  // Numbers for the log (the last Prepare): puffs kept, of dust and of spray drawn; contrails, their pieces.
  int Puffs() const { return static_cast<int>(puffs_.size()); }
  int DustDrawn() const { return groundCount_; }
  int SprayDrawn() const { return sprayCount_; }
  int Trails() const { return trailsDrawn_; }
  int Segments() const { return segments_; }
  int units = 0;  // units looked at in the last frame (AddUnit)

 private:
  // How a kind of unit moves, from its blueprint (once).
  struct Kind {
    enum Move { None, Land, Hover, Air, Water } move = None;
    bool walks = false;       // a walker (its footfalls, or a walking animation): scuffs up less
    float maxSpeed = 3;       // Physics.MaxSpeed, or Air.MaxAirspeed
    std::vector<std::string> engines;  // Display.MovementEffects.Air.Contrails.Bones (lower case)
    // the engines' bones in the model (found by their names the first time with its model)
    mutable const Model* resolvedFor = nullptr;
    mutable std::vector<int> engineBones;
  };
  const Kind& KindOf(const std::string& blueprint);
  // A unit as last seen.
  struct Unit {
    const Kind* kind = nullptr;
    Vec3 place, velocity;
    double clock = -1, seen = -1;
    float carry = 0, ring = 0, trail = 0;  // what is owed of the next puffs (distance; puffs)
    int trails[4] = {-1, -1, -1, -1};      // its contrails now, by engine (into trails_)
  };
  struct Puff {
    float x = 0, z = 0, lift = 0;  // where it was thrown (x, z) and how high over what lies there
    float y = 0;                   // (the height of what lay there: for telling whether the view sees it)
    Vec3 velocity;                 // as thrown (slowed as by air on the card)
    double born = 0;
    float life = 1, size0 = 0.2f, size1 = 1, strength = 0.3f;
    int kind = 0;                  // 0 dust, 1 spray (over water), 2 the faint dust under a hover unit
    float seed = 0;
  };
  struct Point {
    Vec3 at;
    double born = 0;
    float strength = 1;  // by how high and how fast its aircraft was there (0..1)
  };
  struct Trail {
    uint32_t unit = 0;
    bool live = false;  // its engine is still drawing it (the head follows the engine)
    Point head;         // where its engine is now
    float seed = 0;
    std::deque<Point> points;  // the oldest first
  };
  static constexpr int kTrailPoints = 96;  // the most points one contrail keeps (the oldest let go first)
  void Throw(const Puff& p);
  float Random();  // 0..1
  void Contrails(uint32_t id, Unit& u, const Model* model, Vec3 position, const Quat& orientation, float speed);
  bool Init();     // (its shaders, the first time they are wanted: false where they cannot be made)
  void MakeGrid();
  void Draw(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
            float mapWidth, float mapHeight, bool lit, int first, int count);

  gfx::Device* device_;
  const MapData* map_ = nullptr;
  std::string mapPath_;
  std::function<std::optional<std::string>(const std::string&)> readFile_;
  // where the dry ground is dusty: a grid over the map, `cell_` world units a cell (made the first frame it is on)
  std::vector<float> grid_;
  int gridW_ = 0, gridH_ = 0;
  float cell_ = 2;
  bool gridMade_ = false;

  bool on_ = false;
  double clock_ = 0, lastClock_ = -1;
  Vec3 eye_;
  float dryness_ = 1;         // what the weather leaves of the dust (0 wet or raining .. 1 dry)
  float damp_ = 0.5f;         // how cold and damp the air is up high, for the contrails (0.2 .. 1.4)
  std::unordered_map<std::string, Kind> kinds_;
  std::unordered_map<uint32_t, Unit> unitsSeen_;
  std::deque<Puff> puffs_;
  std::vector<Trail> trails_;  // (a trail let go of keeps its place in the list until its last point is gone)
  int liveTrails_ = 0;
  uint32_t seed_ = 0x9e3779b9u;

  int tried_ = 0;  // 0 not yet, 1 made, -1 they cannot be
  gfx::Pipeline puffPipeline_, trailPipeline_;
  gfx::Buffer rows_;
  std::vector<float> rowData_;
  int groundCount_ = 0, sprayCount_ = 0, segments_ = 0, trailsDrawn_ = 0, trailPoints_ = 0;
  int sprayFirst_ = 0, trailFirst_ = 0;  // (rows: the dust's puffs first, then the spray's, then the contrails' points)
  float trailLife_ = 6, trailWide_ = 1.4f;
  float pixelSpan_ = 0.001f;  // (world units a pixel spans one world unit off)
  int debug_ = 0;             // MOHO64_DEBUG_DUST=1: what it read and judged; 2: and its counts every second
  double debugAt_ = 0;
};

// A test of it (the viewer's --movement-dust-test): units of given kinds driven round loops near a place, as a game's
// frame (World::AddFrame draws them, and they raise what they raise).
class MovementDustTest {
 public:
  // `spec`: "uel0201=40,ual0201=10,uaa0303=10" (blueprint ids and how many), round `at` within `radius`; `readBlueprint`:
  // as World's. False with the reason when none can be read.
  bool Init(const std::string& spec, Vec3 at, float radius, const std::function<bool(const std::string&, BpValue*)>& readBlueprint, std::string* error);
  // The frame at `clock` (seconds): each unit on its loop. `ground`: the ground's height; `water`: the water's level (none:
  // very low).
  const Frame& Update(double clock, const std::function<float(float, float)>& ground, float water);
  std::string Describe() const { return describe_; }

 private:
  struct Driver {
    uint32_t blueprint = 0;
    float x = 0, z = 0, radius = 4, speed = 3, angle = 0, turn = 1, elevation = 0;
    bool air = false, hover = false;
  };
  std::vector<std::string> blueprints_;
  std::vector<Driver> drivers_;
  Frame frame_;
  std::string describe_;
};

}  // namespace render
