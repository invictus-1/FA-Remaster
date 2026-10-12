// The world's picture: everything the renderer draws of a map and a game, put together for one
// frame. The viewer (main.cpp) and any other host - the interface's program - use this same class.
//
// A frame goes:
//   device->BeginFrame(...)                       (the host's)
//   world.Begin(camera, width, height, time, clock)
//   ... the host may add things of its own: world.meshes.Add, world.lights.Add, world.particles.Start ...
//   world.AddFrame(frame, armyColours)            a game's units, projectiles and beams
//   world.effects.Queue(events); world.effects.Update(tick, frame)     a game's effects
//   world.Render()                                all passes; the window's pass is left open
//   ... the host may draw over the picture (its interface) ...
//   device->EndFrame(...)                         (the host's)
//
// Between Begin and the next Begin the world also answers where things are on the screen and what
// is under a point of it (Project, GroundAt, UnitAt, UnitsIn).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "ambient_occlusion.h"
#include "shafts.h"
#include "sight.h"
#include "sky.h"
#include "weather.h"
#include "camera.h"
#include "debug_draw.h"
#include "decals.h"
#include "effects.h"
#include "frame.h"
#include "gfx.h"
#include "icons.h"
#include "lights.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "models.h"
#include "particles.h"
#include "post.h"
#include "ribbons.h"
#include "scmap.h"
#include "shadows.h"
#include "terrain.h"
#include "textures.h"
#include "view.h"
#include "water.h"

namespace moho {
class Vfs;
}

namespace render {

class World {
 public:
  struct Options {
    std::string displayTable;        // the engine's tools/dump_display.lua log, or empty (models.h)
    float lodPixels = 0;             // the ground's quad size on screen; 0: the default
    std::string mapDecals = "normals";  // normals | all | albedo | off | corner (main.cpp says what each is)
    std::string decalFilter;         // only the map's decals whose texture's name holds this
    float decalTurn = 1, decalRange = 1;
    bool verbose = false;            // says what it loads
  };

  // `device` and `vfs` (the game's files) must outlive the world. Before the first World is made,
  // gSceneSamples (view.h) must have its final value.
  World(gfx::Device* device, const moho::Vfs* vfs);

  // Builds the shaders and loads the map ("/maps/x/x.scmap"; empty: no map, a test scene).
  bool Open(const std::string& mapPath, const Options& options, std::string* error);
  bool HaveMap() const { return haveMap_; }
  // How the blueprints look as the simulation has them (a running game hands this over once it has
  // started): models and strategic icons then take their names from it instead of from .bp files.
  void SetLooks(const std::vector<BlueprintLook>& looks);
  // The lamps of a unit drawn this frame (a remastered unit's list of lights): lit while the light
  // of the computed sky is low, each where its bone is. AddFrame calls it for a game's units; a
  // host that adds models itself calls it beside `meshes.Add`.
  // `armyColor` (as armies' colours are given, screen values): what a unit's own glow (a light of
  // Kind 'hull') is drawn in; without it, in the list's colour.
  void AddLamps(const Model* model, Vec3 position, const Quat& orientation, const Pose* pose = nullptr, const Vec3* armyColor = nullptr);
  // The unit under the mouse, marked with brackets of a kind: 1 the player's own or an ally's,
  // 2 an enemy's, 3 anyone else's; 0: none. (A selected unit has its own brackets.)
  uint32_t hover = 0xffffffffu;
  int hoverKind = 0;
  // The height of what one stands on there: the ground, or the water's surface over it.
  float GroundHeight(float x, float z) const;

  // --- a frame ---
  // `time`: seconds, for surfaces that move by themselves (water). `clock`: the game's time in
  // seconds, which effects run on (a paused game's effects stand still).
  void Begin(const Camera& camera, int width, int height, float time, double clock);
  // A game's units (as models, with their strategic icons), projectiles and beams.
  void AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors);
  void Render();

  // --- where things are, for the frame last begun (x, y in pixels of its width and height) ---
  // Where a place in the world is on the screen. False when it is behind the camera.
  bool Project(Vec3 world, float* x, float* y) const;
  // The place on the ground (or the water's surface) under a point of the screen.
  bool GroundAt(float x, float y, Vec3* hit) const;
  // The unit under a point of the screen (the nearest one whose body or strategic icon is there);
  // 0xffffffff: none.
  uint32_t UnitAt(float x, float y) const;
  // The units whose middle lies in a rectangle of the screen.
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const;

  // --- what the player's interface shows in the world; each stays as set until it is changed ---
  // The units shown as selected (entity ids): a ring on the ground under each, and its strategic
  // icon in its selected form.
  std::unordered_set<uint32_t> selected;
  // A model shown as a ghost of one colour: a building's preview while its site is chosen.
  struct Ghost {
    std::string blueprint;  // a unit's blueprint id ("ueb0101")
    Vec3 position;
    float heading = 0;      // radians about the upright axis
    Vec3 tint{0.3f, 1.0f, 0.4f};
  };
  std::vector<Ghost> ghosts;
  // Lines between places of the world and rings lying on the ground (order lines, ranges).
  // Colours are 0xAARRGGBB.
  struct Line {
    Vec3 a, b;
    uint32_t color = 0xffffffffu;
  };
  struct Ring {
    Vec3 centre;
    float radius = 1;
    uint32_t color = 0xffffffffu;
  };
  std::vector<Line> lines;
  std::vector<Ring> rings;

  // --- settings, changeable between frames ---
  Look look = Look::Modern;
  bool shadowsOn = true, iconsOn = true;
  float sun = 1.4f, sky = 1.1f, exposure = 0.55f, reflection = 0.7f;  // the modern look's strengths
  // The time of day, in hours (0..24; 6 sunrise, 12 noon, 18 sunset): the modern look's light is
  // then the computed sky's (sky.h). Negative: the map's own light, as the original has it.
  float timeOfDay = -1;
  float cloudCover = 0.3f;  // with a time of day: how much of the sky clouds cover, 0 (none) .. 1 (overcast)
  // The weather, with a time of day: how hard it rains or snows and how thick the air is, each
  // 0 (not at all) .. 1. Rain and snow bring their clouds with them. `wet` and `snowLying` say
  // what the weather has done to surfaces so far (0..1): they follow the rain and the snow in
  // time (wet in a quarter of a minute, dry in two minutes; snow lies in a minute, is gone in
  // three), and a host may set them.
  float rain = 0, snow = 0, fog = 0;
  float wet = 0, snowLying = 0;
  // Weather that changes by itself (weather.h, WeatherAt): with `weatherAuto`, the clouds, rain,
  // snow and fog above are what the seed and the frame's clock (the game's, when a game plays)
  // say. A host gives every player of a game the same seed.
  bool weatherAuto = false;
  uint32_t weatherSeed = 1;
  Climate climate = Climate::Temperate;
  float weatherSpell = 300;  // seconds one kind of weather lasts
  std::string hide;  // kinds of effect left out: any of "particles", "bands", "decals"
  // What the map lies in: mesh blueprints ("/meshes/game/map-border_squ_uef_mesh"), each drawn once
  // around the map as the original places it: at the map's middle, halfway between its lowest and
  // highest ground, not turned, as large as the map is wide times the blueprint's UniformScale.
  std::vector<std::string> borders;

  // --- the parts, for hosts that add to a frame or read its numbers ---
  TextureCache textures;
  MapData map;
  Terrain terrain;
  Water water;
  ModelCache models;
  MeshRenderer meshes;
  Particles particles;
  Ribbons ribbons;
  Decals decals;
  Lights lights;
  Effects effects;
  StrategicIcons icons;
  DebugDraw draw;
  AmbientOcclusion ao;
  Post post;
  Sky skyDraw;  // the computed sky behind everything
  Precipitation falling;  // rain and snow
  SightShade sight;       // what the army the picture is for does not see, darkened (sight.Set)
  Shafts shafts;          // lamps' beams seen in the air
  SkyLight skyLight;  // of this frame, when a time of day is set
  Shadows shadows;
  View view;       // this frame's
  Camera camera;   // this frame's

 private:
  struct PlacedProp {
    const Model* model;
    Mat4 world;
  };
  struct Pickable {  // a unit of the frame, for UnitAt and UnitsIn
    uint32_t id;
    Vec3 centre;
    float radius;
  };
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  bool haveMap_ = false;
  gfx::Texture environment_, sky_, selectionRing_;
  std::vector<PlacedProp> props_;
  std::vector<const StrategicIcons::Kind*> iconKinds_;  // by FrameUnit::blueprint; null: not looked up yet
  std::vector<Pickable> pickables_;
  float time_ = 0;
  float weatherTime_ = -1;  // when wet and snowLying were last moved on
  float lampsOn_ = 0;       // 0 by day .. 1 when it is dark
  // The game's bracket pictures, each cut into its four corners: [0] a selected unit's, [1..3] the
  // kinds of `hoverKind`; corners in the picture's order: top left, top right, bottom left, bottom right.
  gfx::Texture brackets_[4][4];
  bool haveBrackets_ = false;
  void LoadBrackets();
  void AddBrackets(int kind, Vec3 position, const Quat& orientation, float halfX, float halfZ);
  int lampUnits_ = 0, lampUnitsBefore_ = 0;  // units whose lamps were lit this frame, and the frame before
  std::unordered_map<uint64_t, uint32_t> lampSquares_;  // lamps lit this frame per small square of ground: beams, others
  float groundMiddle_ = 0;  // halfway between the map's lowest and highest ground
  double clock_ = 0;
};

}  // namespace render
