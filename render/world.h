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
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "ambient_occlusion.h"
#include "shafts.h"
#include "tracks.h"
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
#include "placed.h"
#include "ranges.h"
#include "wakes.h"
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
  // Life bars over the units that are selected or pointed at, and over damaged ones while they are
  // near enough to be seen as models. Off unless switched on (Chris: off by default; the original's
  // way of reading a unit's life is the panel at the bottom left).
  bool lifeBars = false;
  // Of the frame last begun: every unit with a life bar, and the point under its bars (life, shield,
  // work): the middle of their width and the lower edge of the lowest, in screen pixels as the icons
  // are drawn. Where the original puts a unit's custom name (WorldPicture::LifeBarAnchors).
  struct BarAnchor {
    uint32_t unit;
    float x, y;
  };
  std::vector<BarAnchor> barAnchors;
  // The game's effects as the original has them, or remastered (the visuals track's files where it
  // has made some, effect_files.h): a player's choice, remastered at first (Chris, 2026-10-15).
  enum class EffectsLook { Original, Remastered };
  EffectsLook effectsLook = EffectsLook::Remastered;
  // The effects quality setting: the share of their particles effects throw out (1 all, 0.5 for a
  // slower card; what an emitter's blueprint marks KeepAtLowQuality is thrown out in full).
  float effectsQuality = 1;
  // The share of the screen's width and height the scene is drawn at, FSR 1 bringing it up to the
  // screen (upscale.h; a player's option for slower cards): 1 the screen's own size, 1/1.3 FSR's "ultra
  // quality", 1/1.5 "quality", 1/1.7 "balanced", 1/2 "performance". Icons, bars and the interface stay
  // at the screen's size.
  float upscale = 1;
  // Life bars go over the units of the army the picture is for and of its allies (bit n of
  // `alliedArmies`: army n, counted from 0), as the original's; over the enemy units the army sees
  // now only with `lifeBarsOnEnemies` (the console's ui_ForceLifbarsOnEnemy, FAF's "Always Show
  // Enemy Lifebars", off in the original). With no one army (focusArmy -1): over every unit.
  uint64_t alliedArmies = 0;
  bool lifeBarsOnEnemies = false;
  int dyingDrawn = 0;    // units drawn in the last frame that are in their death (falling, sinking, toppling)
  bool workBars = true;  // the thin bar of a unit's work and the pause mark, also while the life bars are off
  int focusArmy = -1;    // the army the picture is for, counted from 0 (-1: no one army): whose work is shown
  // The height of what one stands on there: the ground, or the water's surface over it.
  float GroundHeight(float x, float z) const;

  // --- a frame ---
  // `time`: seconds, for surfaces that move by themselves (water). `clock`: the game's time in
  // seconds, which effects run on (a paused game's effects stand still).
  void Begin(const Camera& camera, int width, int height, float time, double clock);
  // A game's units (as models, with their strategic icons), projectiles and beams.
  void AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors);
  // Whose a shield's mesh is ("uef", "aeon", "cybran", "seraphim"; "" not known).
  static std::string MeshFaction(const std::string& mesh);
  // How the scenery changed (SimFrames::TakePropEvents): each takes hold when the frame begun has
  // reached its beat. Wreckage appears, a burnt tree turns black, a fallen one lies, what is
  // reclaimed goes.
  void AddPropEvents(const std::vector<FramePropEvent>& events);
  // How the simulation changed the ground's heights (SimFrames::TakeGroundChanges): drawn so at once.
  // Within a frame (the ground's height texture is written).
  void ChangeGround(const std::vector<FrameGroundChange>& changes);
  int propsChanged = 0, propsGone = 0;  // since the map was opened
  int groundChanges = 0;                // the simulation's changes of the ground's heights taken in (ChangeGround)
  int propCellsSeen = 0, propsAdded = 0;  // of the frame last begun: squares of scenery gone through, props handed to the mesh renderer
  int MadeProps() const { return static_cast<int>(madeProps_.size()); }  // props made in the game that stand now
  // Where wreckage lies now (at most `most` places): for looking at it in the viewer.
  std::vector<Vec3> WreckPlaces(size_t most) const;
  void Render();

  // --- where things are, for the frame last begun (x, y in pixels of its width and height) ---
  // Where a place in the world is on the screen. False when it is behind the camera.
  bool Project(Vec3 world, float* x, float* y) const;
  int ScreenWidth() const { return screenWidth_; }
  int ScreenHeight() const { return screenHeight_; }
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
    float width = 1;  // pixels on the screen
  };
  struct Ring {
    Vec3 centre;
    float radius = 1;
    uint32_t color = 0xffffffffu;
  };
  std::vector<Line> lines;
  std::vector<Ring> rings;
  // Meshes the interface's script places in the world (placed.h): rally points, cursor marks,
  // the mark where an order lands.
  std::vector<PlacedMesh> placed;

  // --- settings, changeable between frames ---
  Look look = Look::Modern;
  bool shadowsOn = true, iconsOn = true;
  // The lamps' shadows (lights.h): what stands in a headlight's beam shadows the ground and the
  // units behind it. `lampShadowsDrawn`: how many lamps had a shadow map in the last frame.
  bool lampShadows = true;
  // The sun's shadow maps' size in texels (2048 at first; 4096 sharper, 1024 and 512 for slower cards).
  int shadowSize = kShadowMapSize;
  // The icons on mass and hydrocarbon deposits from far up, where the marks on the ground are no
  // longer drawn (the original's "resource icons", its pictures). `depositIconFrom`: at how many
  // times a mark's own cutoff distance its icon is fully there (it comes in from 0.8 of that).
  bool depositIcons = true;
  // The world beyond the map's edge (modern look): land and sea go on past the edge, a shade
  // dimmer, and are lost in the distance, so the map lies in a world and not on a tray. Nothing
  // is out there for the game: no unit goes there. `borders` (the original's frame of machinery)
  // are not drawn then. false: the map ends at its edge, with its frame, as the original.
  bool worldBeyond = true;
  // The two looks of the map's edge Chris chose between (2026-10-14): MACHINERY, the original's
  // way: worldBeyond false, frame true (the frame whole, nothing past it). NO MACHINERY: worldBeyond
  // true, frame false, beyondHaze false: the land beyond, greyed, with no haze over it but at the
  // very end of what is drawn, where it meets the horizon. `frame`: the original's frame of
  // machinery (`borders`) is drawn. `beyondHaze`: the land beyond is lost in haze from the edge out.
  bool frame = true;
  bool beyondHaze = true;
  int propsBeyond = 0;  // trees and rocks added beyond the map's edge in the last frame
  float depositIconFrom = 1.0f;
  int lampShadowsDrawn = 0;
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
  // The mist that lies on low ground and over water, with a time of day: how much of it, 0..1.
  // Negative: by itself, from the hour and the weather: most at daybreak, some through the night,
  // more on wet ground after rain and in fog, little in a dry climate, none while it snows.
  // `mistNow` is what it came to in the last frame.
  float mist = -1, mistNow = 0;
  // A view for checking art (modern look): what every lit surface shows in place of its lit
  // colour. 0: the picture as it is; 1 colour alone, 2 which way surfaces face, 3 roughness,
  // 4 metal, 5 the creases' shading, 6 the sun's shadow, 7 the small lights alone, 8 the sun's
  // shadow maps by colour, 9 the models' levels of detail by colour.
  int inspect = 0;
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
  Upscaler upscaler;
  Sky skyDraw;  // the computed sky behind everything
  Precipitation falling;  // rain and snow
  SightShade sight;       // what the army the picture is for does not see, darkened (sight.Set)
  bool sightShown = true; // (off for a picture that is darkened by other means: the minimap's ground)
  // The game's option FogOfWar "unexplored": ground the army has never seen is all but black until
  // it is seen. Otherwise (the usual, "explored") it looks like ground seen before.
  bool unexplored = false;
  // Other armies' buildings the focus army remembers though they are gone (destroyed out of its
  // sight): each stays drawn as it was last seen until the army sees the place again. Filled by
  // AddFrame, for hosts that show them elsewhere too (the minimap).
  struct RememberedGone {
    const StrategicIcons::Kind* kind;
    Vec3 position, color;
  };
  std::vector<RememberedGone> rememberedGone;
  int RememberedCount() const { return static_cast<int>(remembered_.size()); }  // all it keeps, gone or not
  Shafts shafts;          // lamps' beams seen in the air
  Tracks tracks;          // where lying snow is trodden away by units (tracks.on switches it)
  Wakes wakes;            // the foam behind what moves on the water
  Ranges ranges;          // range rings, each group as one outline (set ranges.groups)
  PlacedMeshes placedDraw;  // what of `placed` is not an ordinary lit model
  SkyLight skyLight;  // of this frame, when a time of day is set
  Shadows shadows;
  View view;       // this frame's
  Camera camera;   // this frame's

 private:
  struct PlacedProp {  // (model null: not there - its files could not be read, or it is gone)
    const Model* model;
    Mat4 world;
    int cell = -1;  // which square of the map it is listed in (propCells_); -1: none
  };
  struct Pickable {  // a unit of the frame, for UnitAt and UnitsIn
    uint32_t id;
    Vec3 centre;
    float radius;
  };
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  int screenWidth_ = 1, screenHeight_ = 1;  // the picture's size on the screen (the scene's is the view's)
  View screenView_;  // the view at the screen's size (icons and bars are drawn by it)
  bool haveMap_ = false;
  gfx::Texture environment_, sky_, selectionRing_;
  std::vector<PlacedProp> props_;  // the map's, by their index in the map file's list
  std::unordered_map<uint32_t, PlacedProp> madeProps_;  // made during the game (wreckage), by entity id
  // The scenery by squares of the map (kPropCell world units a side): which of the map's props and
  // which made ones stand in each. Only the squares near what the camera sees are gone through
  // each frame: a game breaks a map's tree groups up into tens of thousands of single trees.
  struct PropCell {
    std::vector<uint32_t> ofMap;  // indices into props_
    std::vector<uint32_t> made;   // entity ids in madeProps_
    // its group of still scenery with the model renderer (made the first time it is drawn), and
    // whether the group must be filled anew (a prop of it has changed)
    uint32_t group = 0xffffffffu;
    bool changed = true;
  };
  static constexpr float kPropCell = 32;
  std::vector<PropCell> propCells_;
  int propCellsX_ = 0, propCellsZ_ = 0;
  float propReach_ = 0;  // the largest prop's reach from its place, in world units
  int PropCellAt(const Mat4& world) const;
  void ListProp(PlacedProp* prop, uint32_t key, bool made);
  void UnlistProp(PlacedProp* prop, uint32_t key, bool made);
  std::deque<FramePropEvent> propEvents_;  // told, and not yet due
  const Model* PropModel(const std::string& blueprint, const std::string& mesh);
  void ApplyPropEvent(const FramePropEvent& event);
  std::vector<const StrategicIcons::Kind*> iconKinds_;  // by FrameUnit::blueprint; null: not looked up yet
  // The models of the frame's blueprints by their number in Frame::blueprints, looked up by name once
  // instead of every frame (a lookup makes several strings): a unit's, a projectile's, and a script
  // entity's mesh blueprint. Emptied when the simulation says anew how blueprints look (SetLooks).
  struct KnownModel {
    const Model* model = nullptr;
    bool known = false;
  };
  std::vector<KnownModel> unitModels_, shotModels_, entityModels_;
  std::vector<KnownModel> originalShotModels_;  // (shots a frame wants in the original look whatever the world's)
  std::vector<KnownModel> originalEntityModels_;  // (the same for script entities)
  EffectsLook entitiesLook_ = EffectsLook::Remastered;  // (the look entityModels_ were looked up in)
  std::vector<std::pair<Vec3, std::string>> bubbles_;  // this frame's shield bubbles: their centres and factions
  EffectsLook shotsLook_ = EffectsLook::Remastered;  // (the look shotModels_ were looked up in)
  template <typename LookUp>
  const Model* Known(std::vector<KnownModel>& list, uint32_t index, size_t count, LookUp lookUp) {
    if (index >= list.size()) list.resize(std::max(count, static_cast<size_t>(index) + 1));
    KnownModel& k = list[index];
    if (!k.known) {
      k.model = lookUp();
      k.known = true;
    }
    return k.model;
  }
  std::vector<Pickable> pickables_;
  float time_ = 0;
  float weatherTime_ = -1;  // when wet and snowLying were last moved on
  float tracksCovering_ = 0;  // this frame: how much of the trodden snow new snowfall covers again
  float lampsOn_ = 0;       // 0 by day .. 1 when it is dark
  // where each unit was at the frame before and how fast it went (for the wakes): by entity id
  struct Moving {
    Vec3 place;
    float speed = 0;
    double clock = 0;
    Vec3 trail[12];      // where it was, the newest first, one every third of a second of the game
    int trailCount = 0;
    double trailClock = 0;
  };
  std::unordered_map<uint32_t, Moving> moving_;
  // when each flash on a shield (a script's entity with such a mesh) was first seen, by the
  // frames' clock: its look goes by its age
  std::unordered_map<uint32_t, double> impactBorn_, impactBornNext_;
  int massIcon_[2] = {-2, -2}, hydrocarbonIcon_[2] = {-2, -2};  // the deposits' icons, small and larger (-2: not asked for yet)
  // another army's building as the focus army last saw it: by entity id
  struct Remembered {
    uint32_t blueprint;  // FrameUnit::blueprint
    Vec3 position, color;
    Quat orientation;
    float fractionComplete;
    uint32_t stamp;      // the AddFrame its unit was last in
  };
  std::unordered_map<uint32_t, Remembered> remembered_;
  uint32_t frameStamp_ = 0;
  // The game's bracket pictures, each cut into its four corners: [0] a selected unit's, [1..3] the
  // kinds of `hoverKind`; corners in the picture's order: top left, top right, bottom left, bottom right.
  gfx::Texture brackets_[4][4];
  bool haveBrackets_ = false;
  void LoadBrackets();
  void AddBrackets(int kind, Vec3 position, const Quat& orientation, const StrategicIcons::Kind& unit, const Model* model);
  int lampUnits_ = 0, lampUnitsBefore_ = 0;  // units whose lamps were lit this frame, and the frame before
  std::unordered_map<uint64_t, uint32_t> lampSquares_;  // lamps lit this frame per small square of ground: beams, others
  // MOHO64_DEBUG_LAMPS=<part of a bone's name> (or 1 for every bone): world.cpp, AddLamps
  const char* debugLamps_ = std::getenv("MOHO64_DEBUG_LAMPS");
  int pauseMark_ = -2;  // the game's pause picture among the symbols (-2: not asked for yet)
  int debugLampsTold_ = 0;
  float groundMiddle_ = 0;  // halfway between the map's lowest and highest ground
  float mistFloor_ = 0, mistDepth_ = 3;  // where the mist lies: its floor's height, and over what height it thins to 1/e
  double clock_ = 0;
};

}  // namespace render
