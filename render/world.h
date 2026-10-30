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
#include <atomic>  // [session]
#include <chrono>  // [snowhitch]
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <future>  // [snowhitch]
#include <string>
#include <unordered_set>
#include <vector>

#include "air.h"
#include "beyond_land.h"  // (beyondland)
#include "blasts.h"
#include "build_effects.h"
#include "exhausts.h"  // (unitsfx)
#include "clouds.h"
#include "debris.h"
#include "motion.h"
#include "movement_dust.h"
#include "ambient_occlusion.h"
#include "shafts.h"
#include "tracks.h"
#include "deep_tracks.h"
#include "sight.h"
#include "sky.h"
#include "weather.h"
#include "grass.h"
#include "hidden_outlines.h"
#include "camera.h"
#include "cinecam.h"
#include "cloud_zoom.h"  // (cloudzoom)
#include "dof.h"
#include "debug_draw.h"
#include "decals.h"
#include "effects.h"
#include "energy_shields.h"
#include "frame.h"
#include "frame_generation.h"
#include "gfx.h"
#include "hot_metal.h"
#include "horizon.h"  // [horizon]
#include "icons.h"
#include "lights.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "order_lines.h"
#include "models.h"
#include "particles.h"
#include "machinery.h"  // [machinery]
#include "photo_look.h"  // (photolook)
#include "post.h"
#include "ray_scene.h"
#include "bounce.h"
#include "ribbons.h"
#include "scmap.h"
#include "seasons.h"  // [seasons]
#include "snow_ground.h"  // [snowhitch] (SnowProp)
#include "shadows.h"
#include "smoke.h"
#include "stream_water.h"  // [stream]
#include "sunlight.h"  // [sunlight]
#include "sun_mask.h"  // [shadowmask]
#include "terrain.h"
#include "textures.h"
#include "unit_wear.h"
#include "view.h"
#include "volume_blasts.h"
#include "placed.h"
#include "plants.h"  // [plants]
#include "ranges.h"
#include "wakes.h"
#include "hull_wash.h"  // [hulls]
#include "water.h"
#include "wildlife.h"

namespace moho {
class Vfs;
}

namespace render {

struct ViewSide;  // [views] (views.h)

class World {
 public:
  struct Options {
    std::string displayTable;        // the engine's tools/dump_display.lua log, or empty (models.h)
    float lodPixels = 0;             // the ground's quad size on screen; 0: the default
    // normals | all | albedo | off | corner (main.cpp says what each is); empty: as the map's entry in the remastered
    // ground's remap.json says ("decals"), else normals
    std::string mapDecals;
    std::string decalFilter;         // only the map's decals whose texture's name holds this
    float decalTurn = 1, decalRange = 1;
    // [proplift] the map's props stored below the ground drawn on it, as a game has them (FAF's Prop.lua lifts them as
    // the game starts; a game's own moves come as the simulation's changes anyway): for a map seen without a game, so
    // its picture shows the props where every game puts them. Off: as stored in the map file (as before)
    bool propsAsGame = false;
    bool verbose = false;            // says what it loads
  };

  // `device` and `vfs` (the game's files) must outlive the world. Before the first World is made,
  // gSceneSamples (view.h) must have its final value.
  World(gfx::Device* device, const moho::Vfs* vfs);
  ~World();  // (frame generation let go of: the device no longer calls this world at a frame's end)

  // Builds the shaders and loads the map ("/maps/x/x.scmap"; empty: no map, a test scene).
  bool Open(const std::string& mapPath, const Options& options, std::string* error);
  bool HaveMap() const { return haveMap_; }
  // How the blueprints look as the simulation has them (a running game hands this over once it has
  // started): models and strategic icons then take their names from it instead of from .bp files.
  void SetLooks(const std::vector<BlueprintLook>& looks);
  // [spikes] (readAhead) Unit kinds an army may build (SimFrames::TakeBuildable, from the simulation's own test), made ahead
  // before any of them is first drawn: their textures off the frames' thread, then their models, one a frame
  // (ModelCache::MakeAhead). A host that runs a game hands them over each frame, after SetLooks. Nothing without a map, or
  // with readAhead off.
  void MakeAhead(const std::vector<std::string>& unitIds);
  // [session] The models a game's first frames would make, made now, on the calling thread (a map opened behind:
  // WorldPicture::PrepareSession, on its own thread before the world is shown): these unit kinds' (as the simulation's
  // looks have them: SetLooks first) and the props' as the simulation makes them (blueprint, and the mesh a script set or
  // empty: PropModel). Stops early while `stop` is set.
  void MakeSessionModels(const std::vector<std::string>& unitIds, const std::vector<std::pair<std::string, std::string>>& props,
                         const std::atomic<bool>* stop);
  // The lamps of a unit drawn this frame (a remastered unit's list of lights): lit while the light
  // of the computed sky is low, each where its bone is. AddFrame calls it for a game's units; a
  // host that adds models itself calls it beside `meshes.Add`.
  // `armyColor` (as armies' colours are given, screen values): what a unit's own glow (a light of
  // Kind 'hull') is drawn in; without it, in the list's colour.
  // [nightlight] `id`: the unit's (0: none, a missile's engine), by which its lamps fade in and out over a few frames
  // (steadyLamps); `structure`: a building's lamps go before those of units that move where the ground is crowded.
  void AddLamps(const Model* model, Vec3 position, const Quat& orientation, const Pose* pose = nullptr, const Vec3* armyColor = nullptr,
                uint32_t id = 0, bool structure = false);
  // A unit's outline where it is hidden (hiddenOutlines, hidden_outlines.h), right after its `meshes.Add`. AddFrame
  // calls it for the game's units the player may see; a host that adds units itself calls it beside `meshes.Add`.
  // `color`: its strategic icon's; `building`: a structure (it hides the units behind it); `kind`: its strategic
  // icon (null: none), which the outline gives way to as it comes in with the distance.
  void MarkOutline(const Model* model, Vec3 position, Vec3 color, bool building, uint32_t id, const StrategicIcons::Kind* kind);
  // [whitecaps] Where a unit floating at `position` turned `orientation` is drawn on the lifted waves ([lift] BobAt; world.cpp).
  // [hulls] A ship (its place at the water's level itself, not a hover unit above it) sits a little down in the sea, as deep as
  // a hull of its beam draws (RideWaves' own comment); `rode`: the bob it rode, `sunk`: how far it was sunk (null: not told).
  bool RideWaves(const Model* model, Vec3* position, Quat* orientation, Ocean::Bob* rode = nullptr, float* sunk = nullptr) const;
  // [hulls] A ship RideWaves sank into the lifted sea, listed for where the waves meet its hull (Ocean::AddHull: the water's
  // white wash round it and the hull under the surface hidden; hull_wash.h): `id` the same from frame to frame (its speed is
  // worked out from where it was), its model, its place and turn in the game (not the drawn ones), the bob it rode.
  void ListHull(uint32_t id, const Model* model, Vec3 position, const Quat& orientation, const Ocean::Bob& rode);
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
  // The strategic icons' colours by army (from 0), in place of the armies' own (the game's "team colour"
  // button, TeamColorMode: only the icons change, the models keep their army's colour); empty: the
  // armies' own. A blip not yet known stays as it is.
  std::vector<Vec3> iconColors;
  Vec3 IconColor(uint32_t army, Vec3 own) const { return army < iconColors.size() ? iconColors[army] : own; }
  // The height of what one stands on there: the ground, or the water's surface over it.
  float GroundHeight(float x, float z) const;

  // --- a frame ---
  // `time`: seconds, for surfaces that move by themselves (water). `clock`: the game's time in
  // seconds, which effects run on (a paused game's effects stand still).
  void Begin(const Camera& camera, int width, int height, float time, double clock);
  // A game's units (as models, with their strategic icons), projectiles and beams.
  void AddFrame(const Frame& frame, const std::vector<Vec3>& armyColors);
  // The units of a frame watched for deaths only, for the debris (blastDebris): AddFrame does it for a game's; a host
  // that draws its units itself (the effects stand) hands its frame here, after Begin.
  void WatchDeaths(const Frame& frame, const std::vector<Vec3>& armyColors);
  // Whose a shield's mesh is ("uef", "aeon", "cybran", "seraphim"; "" not known).
  static std::string MeshFaction(const std::string& mesh);
  // How the scenery changed (SimFrames::TakePropEvents): each takes hold when the frame begun has
  // reached its beat. Wreckage appears, a burnt tree turns black, a fallen one lies, what is
  // reclaimed goes.
  void AddPropEvents(const std::vector<FramePropEvent>& events);
  // Deep tracks (deepTracks): a frame's units press the ground as AddFrame has them do, for a host that draws them
  // itself (the effects test stand).
  void PressGround(const Frame& frame);
  // How the simulation changed the ground's heights (SimFrames::TakeGroundChanges): drawn so at once.
  // Within a frame (the ground's height texture is written).
  void ChangeGround(const std::vector<FrameGroundChange>& changes);
  // The hits, impacts and health changes the simulation had since the last call (SimFrames::TakeCombat: render
  // REQUESTS 36), for the looks that answer them (marks on units, ripples, blast waves). A host gives them every
  // frame it takes them, before AddFrame, even when there are none (they also say whether the simulation keeps
  // them). Each is handed to the looks by the AddFrame whose moment has reached its beat (the picture shows a
  // moment up to a beat behind the newest), then let go.
  void AddCombat(FrameCombat&& combat);
  // What AddCombat has said of the host's simulation: whether it keeps impacts (where it does not, splashes in the
  // water and the like are guessed from the frames)
  bool ImpactsKept() const { return combatPending_.impactsKept; }
  int hitsTaken = 0, impactsTaken = 0;  // since the map was opened (AddCombat)
  int hitsShown = 0, impactsShown = 0;  // ... and of those, handed to the looks (their beat reached by AddFrame)
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
  // What GroundAt, UnitAt and UnitsIn answer by between two frames (the camera, the views, the screen's
  // size): kept and put back around a frame drawn for something else (the minimap).
  struct Picking {
    Camera camera;
    View view, screenView;
    int screenWidth = 1, screenHeight = 1;
  };
  Picking KeepPicking() const { return {camera, view, screenView_, screenWidth_, screenHeight_}; }
  void RestorePicking(const Picking& p) {
    camera = p.camera;
    view = p.view;
    screenView_ = p.screenView;
    screenWidth_ = p.screenWidth;
    screenHeight_ = p.screenHeight;
  }
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
  // Pictures the interface lays on the ground (the game's UserDecal: the rings of a weapon's splash
  // while the player aims, a nuke's two rings): a texture of the game's files, its middle and its
  // size across in world units; square to the map (north up), in their own colours, under the units.
  struct UserDecal {
    std::string texture;
    float x = 0, z = 0;
    float sizeX = 1, sizeZ = 1;
  };
  std::vector<UserDecal> userDecals;
  // Meshes the interface's script places in the world (placed.h): rally points, cursor marks,
  // the mark where an order lands.
  std::vector<PlacedMesh> placed;

  // --- settings, changeable between frames ---
  Look look = Look::Modern;
  bool shadowsOn = true, iconsOn = true;
  // The selection's brackets (or ring) and the brackets under the mouse (the console's ren_SelectBoxes:
  // off in a cinematic). On at first.
  bool selectionMarks = true;
  // The part of the map that can be played now (the campaign's SetPlayableArea: an operation opens on
  // part of the map and grows it): x0, z0, x1, z1 in world units; x1 <= x0: the whole map, as at first.
  // Outside it nothing is shown of the game (no unit, icon or bar; it cannot be pointed at) and the
  // picture there is black, as the original's boundary pass makes it (frame.fx Boundary over the
  // stencil of vision.fx's CastBoundary boxes); in the modern look with the world beyond the map's edge
  // it is dimmed instead, behind a dark line along the edge, as that world is.
  float playable[4] = {0, 0, 0, 0};
  bool OutsidePlayable(float x, float z) const {
    return playable[2] > playable[0] && (x < playable[0] || z < playable[1] || x > playable[2] || z > playable[3]);
  }
  // The lamps' shadows (lights.h): what stands in a headlight's beam shadows the ground and the
  // units behind it. `lampShadowsDrawn`: how many lamps had a shadow map in the last frame.
  bool lampShadows = true;
  // [nightlight] Lamps that do not pop (Chris's snapshot test of 2026-10-25, items 3 and 7: lights near factories flickering
  // at night and in fog): a unit's lamps fade in and out over a quarter of a second where they were switched on or off at
  // once (the crowded squares' rule, the most a unit lights, the reach round what the camera looks at), the crowded
  // squares' rule no longer by the order the units come in (buildings first, then lamps lit the frame before, then by the
  // unit's number), and a lamp keeps its shadow map while another is not clearly nearer (Lights::steady). Off: as before.
  bool steadyLamps = true;
  // The sun's shadow maps' size in texels (2048 at first; 4096 sharper, 1024 and 512 for slower cards).
  int shadowSize = kShadowMapSize;
  // [cardcost] The sun's shadow maps draw only what can cast a shadow onto what the camera sees (shadows.h, Shadows::
  // fitCasters): the same picture, what lies outside the view as the sun sees it left out (most of the far cascades'
  // ground and trees at the game's usual heights). On by default; viewer --no-shadow-fit: off (every caster in a
  // cascade's square, as before).
  bool shadowFit = true;
  // [cardcost] The ray tracing scene refitted, not built again, where only where things stand changed since the frame
  // before (ray_scene.h, RayScene::refit; built again every 30 frames and whenever something came or went): the same
  // picture. On by default; viewer --no-ray-refit: off (built every frame, as before).
  bool rayRefit = true;
  // [step2b] The still scenery kept on the card and picked there (MeshRenderer::cardScenery, card_scenery.h; the 3D plants'
  // draws by their lists' counts with it, GroundCover::indirectModels): 0 never (the processor picks it, as at first); 1
  // where it pays (MeshRenderer::cardByNeed: by the props in view, the default); 2 every frame (viewer --card-scenery, to
  // measure). The same picture in all three. Viewer --no-card-scenery: 0.
  int cardScenery = 1;
  // [cardcost] The sun's far shadow cascades drawn anew only every n-th frame, in turn, and kept in between while they
  // still serve (shadows.h, Shadows::keepEvery; the nearest every frame). The ground and the trees cast the same shadows;
  // a unit moving far off casts where it stood a frame or so before. 0 (or 1): every cascade every frame. Viewer
  // --shadow-keep <n>.
  int shadowKeep = 0;
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
  // (beyondland) With the world beyond the edge: a softer edge (the ground past it dims and greys little by little,
  // only a faint soft shadow along the edge) and land far off round the horizon (beyond_land.h, `distantLand`). Modern
  // look; off by default.
  bool beyondLand = false;
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
  // [weather] Fog as a low layer (World::Begin; Chris's snapshot test, 2026-10-25): fog past a quarter lies on the ground and
  // the water in the mist's layer, deeper than the mist, and only a little of it thickens the air all over. Off (the
  // viewer's --fog-old): all of it in the air all over, as before.
  bool lowFog = true;
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
  Machinery machinery{&particles};  // [machinery] (factories' parts at work: machinery.h)
  Ribbons ribbons;
  Decals decals;
  Lights lights;
  Effects effects;
  StrategicIcons icons;
  DebugDraw draw;
  AmbientOcclusion ao;
  Post post;
  // [shadowmask] The sun's shadow on the ground worked out once a visible pixel (sun_mask.h): before the scene, the ground's
  // depth drawn into a target of its own and the shadow maps' share of the sun worked out for each pixel from it, which the
  // ground's pass and the contact shadows read instead of working it out (the rest keep their own). Modern look; off by
  // default (viewer --shadow-mask). The same picture but where a shadow's soft edge is on the ground: there the place a
  // pixel's shadow is looked up at comes from the depth, not from the ground's own shader (another draw of its noise).
  bool shadowMask = false;
  SunMask sunMask;
  // Light in the air (air.h; the lighting upgrade, docs/render-lighting.md): a volume of the air's light
  // over the view, in place of the haze and mist formulas, the sun's shafts and the lamps' beams in it
  // with their shadows. Modern look only. Off by default (Chris, 2026-10-16: each new look a player's
  // option, off until it is measured).
  bool volumetricFog = false;
  Air air;
  // Volumetric clouds (clouds.h; the lighting upgrade): the layer of clouds under a computed sky as real
  // volumes, lit by the sun, in place of the sky pass's own. Modern look; off by default.
  bool volumetricClouds = false;
  // The sky from tables of the atmosphere (sky.h, Sky::BuildTables; the lighting upgrade): light scattered many
  // times over and the planet's shadow, for the sky drawn and for the light fitted to it. Modern look with a
  // time of day; off by default.
  bool skyTables = false;
  // Ray tracing (ray_scene.h; the lighting upgrade's step 4), where the device can: the scene the rays go
  // through, built every frame that something reads it. `rayView`: its check view in place of the picture
  // (viewer --ray-view).
  RayScene rays;
  bool rayView = false;
  // The ray tracing scene made, the first time it is wanted (false where the device has none).
  bool RaysReady();
  // Bounce light (bounce.h; the lighting upgrade, ray tracing): the light the ground, the hills and what stands on
  // them throw back, from probes over the ground round the camera, in place of the sky's fitted light. Modern look,
  // where the device has ray tracing; off by default.
  bool bounceLight = false;
  BounceLight bounce;
  // Reflections by ray tracing (lighting.h under RAYS): where a surface is smooth (metal, wet ground), what its
  // mirrored ray meets in place of the sky. Needs gRayShaders (set before the world is made); modern look; off by
  // default.
  bool rayReflections = false;
  // The small lights' shadows by rays (lighting.h, SmallLightsTraced): explosions' flashes, muzzle flashes, shots and
  // lamps are shadowed by what stands between (units, scenery, leaves by how much of them is leaf, the hills; not the
  // water), each place casting at most `rayLightRays` rays (1..4; grass one, the water two, leaves none; more: one
  // for every light, exact, for comparing), the lights for them chosen by their unshadowed light. The lamps with shadow
  // maps of their own (the eight nearest) keep them. Needs gRayShaders, as rayReflections (WorldPicture: from the next
  // map opened); modern look; off by default. `rayLightSize`: a light's own size as a share of its radius (its flash
  // is not shadowed by what lies inside that ball: the wreck a fire burns in), `rayBeamSize` a lamp's beam's.
  bool rayLightShadows = false;
  int rayLightRays = 4;
  float rayLightSize = 0.2f, rayBeamSize = 0.02f;
  // Rain and snow lit where they fall (weather.h): each drop or flake by the sun through its shadow and by the lamps
  // and their beams through their shadows, sent on mostly forward, so rain shows in a headlight's beam and shines
  // toward a light. Modern look; off by default.
  bool litRain = false;
  // Rain's splashes (weather.h): where rain meets what faces up (the ground, a unit's top) and the picture shows it,
  // small crowns that rise and fade. Modern look; off by default.
  bool rainSplashes = false;
  // Wet shorelines and caustics (terrain.cpp, water.cpp): the ground just above the water darker and shinier where
  // the waves have wetted it, and the bright net of light on a shallow bed sharper and stronger. [shore] And the sun's
  // glint on the water widened and dimmed by how much water a pixel covers (water_glint.h: from far up a calm sheen, not
  // single pixels twinkling); with the surf on, the wet beach follows the surf (surf.h, SurfWetAt; water.cpp, WetBeach:
  // wider and darker where it runs up an exposed beach, narrow in a calm bay, a film glistening after each wave), the
  // ground's own band by height narrowed meanwhile. Modern look, a map with water; off by default.
  bool waterDetail = false;
  // Ripples on the water (ripples.h; water.AddRipple): shells landing in the water, ships, hover units and units
  // wading leave rings and wakes that spread, cross each other and die out. Modern look, a map with water; off by
  // default.
  bool waterRipples = false;
  // [wakes] The ships' wakes remade (wakes.h; Chris's test run of 21 October): the foam behind every ship drawn so that it
  // holds for a ship of any size and speed - arms that are lines of foam, streaks that keep their place on the water at
  // the screen's resolution, a wake that runs out softly (the way kept behind a ship spaced by its wake's length, so a
  // large slow ship's covers it). Modern look; off by default (then, and in the original look, the wake as before).
  bool wakesRemade = false;
  // Winter water (winter_water.h): while snow lies (snowLying) the water goes over to a cold palette (deep water a dark
  // slate, the shallows a muted grey-green, the bed dimmer and colder), slush and thin ice gather along the shores, the
  // foam greys; and no snow lies under the water. It follows snowLying, so a thaw brings the summer water back. With no
  // snow lying, nothing changes. Modern look with a time of day, a map with water; off by default (in the game's
  // "Weather Effects").
  bool winterWater = false;
  // [surf] Surf and sea spray (surf.h, water.surf): waves run in to the map's shores, break into white water that runs
  // up the beaches and drains back, and burst on the rocks, throwing spray up them; ships at speed throw spray off their
  // bows. Stronger in rain and most in a storm (the weather's wind), calm on a still day. `surfRoughness`: how rough the
  // sea is, 0 (still) .. 1 (a storm), for a host that sets it itself; -1: by the weather. Modern look, a map with water;
  // off by default (proposed for the game's "Weather Effects").
  bool surf = false;
  // [ocean] A real ocean (ocean.h, water.ocean; Chris, 2026-10-23): the modern water's waves worked out on the card from a
  // wind sea's spectrum with a little long swell (an FFT ocean: three squares of waves from long to short), in place of the
  // four scrolling wave pictures: the waves' slopes, their whitecaps where crests fold (foam that drifts downwind and draws
  // out into streaks), the sun's glitter clean at every distance, a glow through the crests lit from behind. As rough as
  // the weather's wind (the surf's measure: calm, rain, a storm; `surfRoughness` for a host that sets it), calmer on lakes,
  // in bays and over shallows. The sheet stays flat: everything on the water works as before. Modern look, a map with
  // water; off by default (proposed for the game's "Weather Effects", or a "Water" quality of its own).
  bool oceanWaves = false;
  // [lift] Waves that lift the surface (ocean.h; with oceanWaves): the water drawn as a grid that follows the camera, lifted
  // and shifted by the same waves (knee-high on a small unit in a storm, less in a calm and in bays, none over the
  // shallows, where the surf and the beach keep the flat sheet's waterline); ships and hover units ride them in the picture
  // alone (they rise, fall, pitch and roll with the long waves under them: the game's water stays flat, picking and weapons
  // as they were). Off by default.
  bool oceanLift = false;
  float surfRoughness = -1;
  // Ground cover near the camera (grass.h): grass blades on grassy ground, coloured from it, swaying in the wind,
  // low and gone with the distance (nothing when the camera is far). Modern look; off by default.
  bool groundCover = false;
  // [plantlod] With the ground cover (plant_lod.h; Chris's test run of 21 October): its plants by their size on screen over
  // all the ground in view, no longer gone beyond the cover's square (each down to a few pixels across, a clump drawn as
  // fewer plants once small, and the ground taking their colour where they are no longer drawn: GroundCover::plantsFar);
  // their cards fanned out and rounded, to read with volume from the game's high camera (plantsRound); the visuals
  // session's 3D plants where the cover has them, by size on screen down to their card (plantModels, with plantsFar).
  // Off by default (proposed for the game's Ground Detail).
  bool plantsFar = false, plantsRound = false, plantModels = false;
  // Battle scars (decals.h, Decals::battleScars): the scorches and craters of the fighting stay on the ground,
  // weathering, up to a most for cost. Off by default (the game's own times).
  bool battleScars = false;
  // [scars] The marks' relief lit as the ground (decals.h, Decals::modernRelief; Chris, 2026-10-20): the normal decals
  // (craters' dents, the remastered map's trenches and old craters) lit by the modern light, so that they keep their
  // look by night (no dark squares, white rims, doubled rings), soften under lying snow, and the map's plain normal maps
  // apply as such (no dark strip beside every trench). Modern look; off by default (the ground detail's).
  bool markRelief = false;
  // [decalfade] How the decals fade out with distance (decals.h, Decals::fadeFraction; ui's request 33), in both looks.
  // 0..1: as the original (CWldTerrainDecal::GetLODAlpha, measured by the camera's width at the decal's depth times its
  // LOD scale, Camera::lodScale); the value is the console's ren_DecalFadeFraction: 0.5 the stock game's, 0.25 what FAF
  // sets at every game's start. Negative (as at first): as before this, by the distance from the eye, faded over the
  // last fifth of it - which differs from the original's at 0.5 (its decals go at a depth of the cutoff over the view's
  // width at a depth of 1: about 0.6 of that distance at the viewer's field of view on a wide picture, and fade from half
  // of it), so the default keeps today's picture; the host sets the game's.
  float decalFadeFraction = -1;
  // Life in the world (wildlife.h, wildlifeDraw): flocks of birds over the woods and gulls over the shores, that scatter
  // when fighting comes near and settle again after a quiet while, and fireflies at dusk and by night along the woods'
  // edges and over grass. Purely for the eye. Modern look; off by default.
  bool wildlife = false;
  // Light through leaves (lighting.h, LeafLight): the sun's light that falls on the far side of leaves, grass
  // blades and the plants comes through, deeper in colour, and glows toward an eye that looks toward the sun through
  // them (without it leaves let some through, plainly, and grass and plants none). Modern look; off by default.
  bool leafLight = false;
  // Lit smoke (particles.h, smoke.h; Chris, 2026-10-17): smoke and dust lit as volumes in the modern look: by the sun
  // through the shadows of what stands in its way (hills, trees, the clouds, the plume itself: dark deep inside a
  // column, bright on its sunny side), by the small lights (explosions' flashes; and lasting fires, which with it
  // are small lights too, flickering, in the remastered effects' look: `fireLight` how bright), thinner and brighter
  // at the edges toward the sun. Off by default.
  bool litSmoke = false;
  float fireLight = 1.0f;
  // [smoke air light] Lit smoke in fog and at night lit by the air's own light too (particles.h, Particles::airLight;
  // Chris's snapshot test of 25 October, item 9: an anti-nuke's exhaust smoke rising in fog at night read as a dark line
  // drawn up off the map): smoke never reads darker than the air it hangs in, unless it is dark smoke. Only with litSmoke
  // (its pipelines); -1 (the default): with litSmoke, as the game's battle effects turn it on; 0 off; 1 on. By day in
  // clear air nothing changes.
  int smokeAirLight = -1;
  // Smoke's shadow (smoke.h): the smoke darkens the sun's light on the ground, the units, the water and in the light
  // in the air under it. Modern look; off by default.
  bool smokeShadows = false;
  SmokeShadowMap smokeMap;
  // Heat shimmer (particles.h): the air bends what is behind it over fires, engines' exhausts and fresh explosions,
  // a few pixels at most. Modern look; off by default.
  bool heatShimmer = false;
  // Blasts (blasts.h, debris.h; Chris, 2026-10-17): what the game's explosions do round them, to be seen only.
  // `blastWaves`: a ring runs out from each blast over the ground, pressing the grass and plants flat away from it and
  // bending the trees, which then spring back; `blastDust`: a bigger blast kicks a low ring of dust off the ground;
  // `blastDebris`: a dying unit throws a handful of small chunks that tumble, bounce and stay as rubble (up to a most,
  // the oldest sinking away). Modern look; off by default.
  bool blastWaves = false, blastDust = false, blastDebris = false;
  Blasts blasts;  // the blasts of the last half minute (blasts.Recent(): for the water's ripples), their waves and dust
  Debris debris;  // the chunks and the rubble
  // Deep tracks (deep_tracks.h, `trackField`): where units drive and walk, the ground is pressed by the kind it is:
  // troughs and berms in snow, dark wet ruts in mud, compacted dirt, soft sand with ridged edges, grass laid flat and
  // worn to the earth; fading with the weather and the ground. The game's tread marks and footprints are pressed in
  // by their own shapes instead of laid as pictures. Modern look; off by default.
  bool deepTracks = false;
  // [parallax] The ground's relief close by (terrain.h, Terrain::parallax; Chris, 2026-10-19): on a remastered map, the
  // ground's stones, pebbles and clods stand up out of it and its hollows sink as the eye passes, from the height its
  // materials' scans hold; with a low sun its high places shade the hollows. Only near the eye at close zoom (nothing
  // from the game's usual height). Modern look; off by default (proposed for the "Ground Detail" setting).
  bool groundParallax = false;
  // [tarmac] Remastered tarmacs (tarmacs.h, Decals::remasteredTarmacs; Chris, 2026-10-19): the pads under buildings whose
  // picture the visuals track has remade (remap.json's "tarmacs": the UEF pads first) drawn in its material: lit with the
  // modern light, wet and glossy in rain with puddles in their joints, their relief close by with groundParallax, scuffed
  // by deep tracks, scorches and tread marks on them as on the ground, the ground cover cleared under them; faded as the
  // game fades a dead structure's pad; its 8k tier with the ground's (terrain.ultraGround). The original look and the
  // original effects keep the game's pads, as do pads not remade yet. Modern look with the remastered effects; off by
  // default (proposed for the "Ground Detail" setting).
  bool remasteredTarmacs = false;
  // [hitches] What a game will want later made ahead, so that no frame waits for it (Chris, 2026-10-19: "upgrading to t2 on
  // uef factory causes momentary lag spike", "sometimes zooming in suddenly causes lag spikes"): the next tier of each
  // structure kind as the kind appears (ModelCache::ReadAheadUpgrade: its files read on a thread, its textures and model
  // made a little each frame), the remastered pads' materials as soon as a pad in them is laid anywhere (Tarmacs::Want),
  // and the pipelines first wanted later in a game made the first frame their looks are on (the build sparks, the pads,
  // the ground cover left out of the ambient occlusion, the surf's spray, lit rain and rain's splashes, the wet ground:
  // PrepareAhead). What is made ahead is what its first use would have made, so the pictures are the same. On unless
  // switched off (the viewer's --no-read-ahead). [snowhitch] (Chris, 2026-10-21: "froze for around 5-10 seconds right before
  // it began to snow"): while the game's weather can snow (SnowAhead), the snow's too, the first frame (PrepareSnow: the
  // ground's snow materials read with the wet ground's, the snow's pipelines, the shelter's props on a thread of their own);
  // the pipelines not wanted yet (the snow's, the wet ground's, the low plants' under snow) made on the device's own thread
  // (gfx.h, BeginBackgroundPipelines), so that neither the first frame nor the snow's waits for the driver.
  bool readAhead = true;
  // [ground mud] The map's painted mud (terrain.h, Terrain::mudMask; Chris and the visuals session, 2026-10-19): where the map's
  // remap.json entry names a mud mask (SETONS_R's stream banks), its mud material laid over the ground by it, wet and glossy in
  // rain with puddles in it, drying paler and cracked; deep tracks press into it as mud, no plants grow on it. Modern look;
  // off by default here, in the game's "Ground Detail" (world_picture.h).
  bool mudMask = false;
  // [rain ground] The rain-wet ground (terrain.h, Terrain::rainGround): where the map's entry lists wet twins of its layers'
  // materials, the ground goes over to them as the weather wets it, the hollows first, and back as it dries. Modern look with
  // the weather; off by default here, in the game's "Weather Effects" (world_picture.h).
  bool rainGround = false;
  // [snow ground] The lying snow from the map's snow materials, shaded by what is round it (terrain.h, Terrain::snowGround;
  // Chris, 2026-10-19 and 2026-10-20: "the map looks like a blanket"): where the map's entry names snow materials, the snow's
  // colour, small shapes, creases and relief are theirs, drifted on slopes away from the wind and ridges, packed where it is
  // trodden, breaking up by the ground's height where it thins, slushy at the thaw; on any map, the snow darker in valleys,
  // hollows, under and beside trees and rocks (its shelter: the map's heights and props, made once the first time snow lies),
  // deeper in the wind's lee and hollows, thinner and patchier under trees and on steep slopes. Modern look with the weather;
  // off by default here, in the game's "Weather Effects" (world_picture.h).
  bool snowGround = false;
  // [undersnow] The map's low plants under lying snow (under_snow.h, MeshRenderer::underSnow; Chris, 2026-10-21): its ferns,
  // bushes, weeds and tufts placed as props stand in the same snow as the ground cover (GroundCover::snowDepth, with the
  // snow ground's shelter where it is on): sunk by its depth, pressed and bowed by it, the lowest buried first, what stands
  // out white where the ground round it is; the trees and rocks keep their snow. Modern look with the weather; off by default
  // here, in the game's "Weather Effects" (world_picture.h).
  bool plantsUnderSnow = false;
  // [leftover] Dry ground matt at grazing light (terrain.h, Terrain::mattGrazing): at dusk and dawn the sun's highlight on dry
  // ground fades as the sun sinks (the soil between the grass blades no longer shows a sheen like wet plastic); wet ground,
  // puddles and lying snow keep theirs. Modern look; off by default here, in the game's "Ground Detail" (world_picture.h).
  bool groundMatt = false;
  // [plants] Plants that react (plants.h, `plantLife`; Chris, 2026-10-18): grass and the plants laid over where units drive,
  // rising again over seconds; bushes, ferns, saplings and the like bending under them and springing back; the grass, the
  // plants and the undergrowth burnt black, then brown and lower where explosions and fires were, growing back over
  // minutes; the trees the game fells no longer swaying, the ones it burns charred skeletons. Modern look; off by default.
  bool plantsReact = false;
  // [energy shields] Shields that look like energy (energy_shields.h; docs/render-lighting.md, item 11): a shield's
  // bubble bends what is behind it, rings run across it from each hit, its pattern flares where it is hit and as it
  // weakens, and its light falls on what is inside it. Drawn by `energy` (at the end of the class) in place of the
  // plain bubble (not for the effects stand's original side). Modern look; off by default.
  bool energyShields = false;
  // [lampcolour] (Chris, 2026-10-28) a unit's own faint glow (its lamps of kind 'hull') in its army's colour (true, as
  // before), or in its faction's own, the colour its lamps' file gives (UEF blue whatever its army's colour)
  bool lampsTeamColour = true;
  // Battle scars on units (unit_wear.h): each unit keeps marks of the hits it survived, of the kind of what hit it,
  // in three tiers by all the damage it has taken, mended (not gone) as it is repaired. Modern look; off by default.
  // Snow on units (unit_wear.h): each unit holds its own share of the lying snow, which builds up while it stands
  // in snowfall and comes off as it moves, fires, is hit, burns or a blast is near. Modern look; off by default.
  bool unitScars = false, unitSnow = false;
  // [unit dirt] Wet and muddy units (unit_wear.h): each unit its own wetness (soaked in rain, a film of water and drops
  // running down, drying over minutes, faster driving and in sun), mud on its running gear and lower hull where it
  // drives through mud or wet dirt (fresh dark and glossy, drying lighter, flaking off, washed by rain) and dust from dry
  // sand and dirt; none of mud or dust on hover units and aircraft. Modern look; off by default. `unitDirtPace`: all its
  // changes that many times faster (the viewer's --unit-dirt-pace, to look at them).
  bool unitDirt = false;
  float unitDirtPace = 1;
  // [grime] Battle grime and weather dirt on units, and their damage shown by their health (unit_grime.h): dust, caked mud
  // and soot gathering as a unit fights and drives, mud splashed up it in rain, frost on it where snow lies (unitGrime);
  // dents, burns and torn plates showing as its health drops, hot where fresh, patched as it is repaired (unitDamage). With
  // the visuals session's art where a unit has it (its masks and damage set, ModelCache::wearArt), else by its shape.
  // Modern look; off by default (proposed: with the Battle Effects).
  bool unitGrime = false, unitDamage = false;
  UnitWear wear;
  // A game's beat for them: AddFrame calls it; a host that draws units itself (the effects stand) calls it with its
  // frame first. Only the first call for a beat counts (the dirt's: every call, by the clock).
  void WatchWear(const Frame& frame);
  // What a unit (its entity id) wears this frame, for MeshRenderer::Wear; null: nothing of its own. ([unit dirt]
  // `drawn`: the model a host that draws the unit itself draws it with, UnitWear::Look)
  const WearLook* WearOf(uint32_t id, const Model* drawn = nullptr) {
    wear.scars = unitScars;
    wear.snow = unitSnow;
    wear.dirt = unitDirt;  // [unit dirt]
    wear.grime = unitGrime, wear.damage = unitDamage;  // [grime] (and the weather on them: frost, wet)
    wear.frostLying = timeOfDay >= 0 ? snowLying : 0.0f, wear.groundWet = timeOfDay >= 0 ? wet : 0.0f;
    return look == Look::Modern && (unitScars || unitSnow || unitDirt || unitGrime || unitDamage) ? wear.Look(id, clock_, drawn) : nullptr;
  }
  // Units seen behind trees and cliffs (hidden_outlines.h; docs/render-lighting.md, item 15): where a unit the player
  // may see (his own, an ally's, an enemy's in sight; never one in the fog of war, nor a building only remembered) is
  // hidden behind leaves, props, a hill or a building, a faint thin outline in its strategic icon's colour shows
  // where it is. Modern look; ON by default (Chris via the interface session, 2026-10-17: readability); the
  // original look has none, as the original game had none.
  bool hiddenOutlines = true;
  HiddenOutlines outlines;
  // Hot metal (hot_metal.h; renderer item 12): fresh wrecks glow where they broke and on their thin parts and cool over
  // a minute or two, fresh scorches and craters glow in the middle, cooling wrecks smoulder; a faint light from the
  // hottest few. Timed by the game's clock from when each appeared. Modern look; off by default.
  bool hotMetal = false;
  HotMetal heat;
  // What a wreck the host draws itself (the effects stand's) hands MeshRenderer::Add as its `tint` to show its heat:
  // `key` its own number, the same every frame (its age counts from the first frame it is asked for). Null when hot
  // metal is off or the wreck has cooled. Between Begin and Render.
  const Vec3* WreckHeat(uint32_t key, const Model* model, const Mat4& world);
  // (movement dust) Dust, spray and contrails of what moves (movement_dust.h; Chris, 2026-10-17): vehicles raise dust on
  // dry ground (none on grass, wet ground or in rain), hover units throw spray over water, high aircraft draw contrails.
  // Modern look; off by default.
  bool movementDust = false;
  MovementDust dust;
  // Explosions as real volumes (volume_blasts.h; Chris, 2026-10-17, the next looks' item 13): big blasts drawn as lit
  // volumes played back from the visuals session's simulations, glowing while hot; a nuke's flash lighting the sky and
  // the map, its shock ring through the volumetric clouds, the ground's glow under it, its mushroom cloud rising and
  // lingering. Modern look; off by default.
  bool volumeExplosions = false;
  VolumeBlasts volumes;
  // The remastered build look (build_effects.h; Chris's list of 17 October, look 19): build beams light what they work on,
  // sparks fly from where they meet it and bounce off what the picture shows, and units under construction take a better
  // build shimmer of their faction's. Modern look with the remastered effects (the original effects keep the game's build
  // look); off by default.
  bool remasteredBuild = false;
  BuildEffects build;
  // (unitsfx) Real exhausts (exhausts.h; Chris's playthrough of 20 October, item 8): engines' beams drawn as soft plumes
  // of a white-hot core, a sheath of their colour and a deeper fringe, with shock diamonds on large drives at full burn,
  // a light on the hull and the ground, heat haze behind, the throttle followed smoothly. Modern look; off by default
  // (each exhaust the game's band, as before).
  bool realExhausts = false;
  Exhausts exhausts;
  Clouds clouds;
  // (cloudzoom) Zooming out through the clouds (cloud_zoom.h; a trial): with the volumetric clouds, the game's camera
  // rises through their layer as it zooms out, and they lie below it, thinning so the map stays readable. Modern
  // look; off by default. `cloudZoomDraw`: its look (CloudZoom's numbers) and the pass that lays them over the scene.
  bool cloudZoom = false;
  CloudZoom cloudZoomDraw;
  // Soft shadows (lighting.h, SunShadow; the lighting upgrade): the sun's shadows sharper near what casts
  // them and blurred farther off, as a sun that is not a point makes them. Modern look; off by default.
  // `sunSize`: how much wider an edge grows per world unit from its caster (0.02: twice the real sun's).
  bool softShadows = false;
  float sunSize = 0.02f;
  // Contact shadows (ambient_occlusion.h; the lighting upgrade): small shadows in creases and under parts,
  // too small for the sun's shadow maps, from a short march toward the sun along the scene's depth.
  // Modern look; off by default.
  bool contactShadows = false;
  // [step2b] The ambient occlusion and the contact shadows worked out at half the view's width and height and brought to
  // its size by the depth (ambient_occlusion.h, AmbientOcclusion::half): a quarter of their work, the picture within a few
  // levels of every pixel's (4K: 1.1 -> 0.5 ms of card). Off by default here (every pixel, as before); in the game's
  // "Soft Shadows" bundle (world_picture.cpp); viewer --ao-half.
  bool halfOcclusion = false;
  // Motion (motion.h): where each pixel was the frame before, for the temporal upscaler; it needs the
  // scene drawn with one sample a pixel (gSceneSamples 1). `motionView`: a view for checking it (1 the
  // motion as colours, 2 the frame before carried to this one; Motion::Check; 5 the temporal upscaler's
  // reactive mask, Post::Reactive, in red).
  bool motionOn = false;
  int motionView = 0;
  Motion motion;
  // [leftover] What sways and the ground cover tell the motion how they move (MeshRenderer::swayMotion, GroundCover::DrawMotion):
  // the trees and bushes in the wind, the blasts' waves and plants that react; the blades, tufts and the plants' cards. On
  // whenever the motion is drawn (the temporal upscaler, frame generation); false (the viewer's --no-plant-motion): as
  // before, they move with the camera's part alone, and smear under the temporal upscaler.
  bool plantMotion = true;
  // The glow round what is brighter than the screen can show, in the modern look (the game's Bloom
  // Render and Bloom Intensity): off, or this strong (1.5, the game's default, is the modern look's own).
  bool bloom = true;
  float bloomIntensity = 1.5f;
  Upscaler upscaler;
  // AMD's FSR 3 (fsr3.h; the lighting upgrade's groundwork) in place of FSR 1: the scene drawn a little
  // shifted every frame and brought up to the screen's size (`upscale`) from what the frames before saw
  // there; at `upscale` 1 it smooths the edges at the screen's own size (in place of MSAA). Needs one sample
  // a pixel (gSceneSamples 1); without it, or where its shaders cannot be made (Direct3D's own compiler),
  // FSR 1 stays. A player's option, off by default. Its shaders are made the first time it is on.
  bool temporalUpscale = false;
  // A cut, for the temporal upscaler: the camera jumped (set by the host before the frame; Begin clears it).
  // A camera that moves far in one frame, a map opened and the option just switched on count as one too.
  bool cameraCut = false;
  // (airfix) A weather cut: the light in the air and the volumetric clouds start again from this frame (Air::Restart,
  // Clouds::Restart) where they would have taken half a second to forget the light before, so the window shows the new
  // light at once. Begin makes one itself when the moment or the weather jumps between two of the window's frames (the hour
  // by more than half an hour, the clouds, rain, snow, fog, wet or mist by more than a tenth, a time of day on or off: the
  // photo mode's Clear, a key that steps them; the photo mode's own easing over a second does not); a host that knows of
  // one may set it before the frame too (Begin clears it). Photos and side pictures keep their own (world_picture.cpp).
  // `weatherCuts` false: none (as before; to compare).
  bool weatherCut = false;
  bool weatherCuts = true;
  // A frame drawn for something else between two of the game's (the minimap's ground): no temporal upscaler,
  // no motion, no light in the air and no volumetric clouds, and what they keep of the frames before is left
  // for the game's next frame.
  bool sidePicture = false;
  // [views] A view of the frame besides the first (WorldPicture::SetViews, views.h): the same moment drawn again from
  // another camera, after the first view's Begin, AddFrame and Render, with the same time and clock. Nothing of the world
  // moves on a second time in it: what the first view's frame moved on, pressed, threw or kept round its camera is drawn
  // as it is (the deep tracks' field, the ripples', the ocean's foam, the plants' press, the build sparks, the hot wrecks'
  // smoke; the read-ahead). What a view keeps of its own frames before (the temporal upscaler, motion, the light in the
  // air, the volumetric clouds, the bounce light's probes) and its pictures at its size are its own, swapped in by
  // SwapView around its frame. With sidePicture too: an inset, drawn as a side picture is (no temporal history, no light
  // in the air, no volumetric clouds), with the interface's marks in it (order lines, hidden outlines) as a view's.
  bool extraView = false;
  void SwapView(ViewSide& side);  // [views] (views.cpp: the view's own parts in, the ones in the world out; again: back)
  // (cinecam) A frame drawn for a photo (WorldPicture::TakePhoto, the viewer's --photo): the world as the game's
  // frame draws it, at the photo's size and the scene's own (no FSR), but without what keeps frames before for the
  // game's next (the temporal upscaler, motion, frame generation's picture), and with nothing of the interface laid
  // over it (icons, bars, order lines). What else of the interface is in the world the host takes out itself.
  bool photo = false;
  // [session] A warm frame of a map opened behind with the game's first beat (WorldPicture::PrepareSession): drawn as the game's
  // frames are, its icons, life bars and hidden outlines too, though a photo's frame (so that what they make the first time is
  // made unseen). Off at first.
  bool warmAll = false;
  // (photomode) A frame drawn for a tile of a photo (WorldPicture::StartPhoto, photo.h): the photo's own camera, round
  // which what is placed for the camera is placed in every tile of it (the sun's and the lamps' shadows, the grass, the
  // rain's box, the bounce light's probes, the smoke's shadow, lightning, deep tracks, the hot things' lights), so that
  // the tiles agree where they meet. Off at first.
  bool photoTiled = false;
  Camera photoWhole;
  const Camera& Placing() const { return photoTiled ? photoWhole : camera; }
  // (photofix) A photo's frame as the window shows it: `photoPixel` of this frame's pixels make one of the window's (the
  // rain's and the snow's widths and the splashes' least size are measured in the window's pixels), and the frame shows
  // `photoShare` of the window's view (a tile, part of it). `photoMeter`: the photographic look's eye meters this frame and
  // is set to it at once (a photo whose moment the window had not settled into yet: the window's eye was not yet there).
  // 1, 1 and false outside a photo's frames (WorldPicture::PhotoStep sets them for its frames only; photofix2: TakePhoto's
  // set the first two too, and the ambient occlusion's reach is measured by photoPixel as well).
  float photoPixel = 1, photoShare = 1;
  bool photoMeter = false;
  // (photomode) A moment of the picture's own (WorldPicture::SetPhotoMoment): while `on`, each frame shows this hour and
  // this weather in place of the game's, in the modern look, also where the map's own fixed light is used (the day is
  // then on for the picture). The game's own go on underneath: its weather by the clock (weatherAuto), what that does to
  // surfaces by the time, the hour as the host sets timeOfDay; between frames timeOfDay, cloudCover, rain, snow, fog,
  // wet, snowLying and weatherAuto hold the game's own. Off at first: everything as before.
  struct Moment {
    bool on = false;
    float hour = 12;                                   // 0..24
    float clouds = 0.3f, rain = 0, snow = 0, fog = 0;  // as cloudCover, rain, snow, fog
    float wet = 0, snowLying = 0;                      // what the weather has done to surfaces, 0..1
  };
  Moment moment;
  // (cinecam) The cinematic camera's lens (cinecam.h): its depth of field, on the finished light before the glow and
  // the tone curve (dof.h), in either look. Off at first.
  Lens lens;
  DepthOfField dof;
  // (cinecam) With a cinematic camera (Camera::cinematic) and a time of day: the least haze in the air (the share
  // of light lost per world unit, as the weather's; 0: none, as for the game's camera), which clear weather has none of.
  float cinematicHaze = 0.0004f;
  // With it: what is added to the level of detail the textures are read at, beyond log2 of the share drawn
  // (0: the share alone, the textures as fine as at the screen's own size). AMD advises -1 more; measured on
  // Seton's at 1080p from 720p it moves the picture further from today's at the screen's size and flickers
  // a third more where nothing moves (2026-10-16), so 0.
  float temporalMipBias = 0;
  Fsr3 fsr3;
  bool TemporalNow() const { return temporalNow_; }  // this frame brought up by FSR 3
  // AMD's FSR 3 frame generation (frame_generation.h): a picture made between each two frames from the scene's
  // depth and motion and the finished pictures, and shown between them, evenly apart (gfx.h,
  // Device::SetFrameGeneration): twice the frames shown for a little more than half a frame's delay. The icons and
  // the interface are taken from the frame after (the finished picture before them is kept for that,
  // Post::KeepFinished), so they never smear. Needs one sample a pixel (gSceneSamples 1), as the temporal upscaler,
  // with or without it; where its shaders or the device cannot, nothing changes. From the frame after it is
  // switched on. A player's option, off by default.
  bool frameGeneration = false;
  FrameGeneration framegen;
  bool FramesGenerated() const { return framegenNow_; }  // this frame has a picture made before it
  Sky skyDraw;  // the computed sky behind everything
  Precipitation falling;  // rain and snow
  GroundCover grass;      // ground cover near the camera (groundCover)
  Wildlife wildlifeDraw;  // birds and fireflies (wildlife)
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
  DeepTracks trackField;  // deep tracks (deepTracks): the field of where the ground is pressed
  PlantLife plantLife;    // [plants] (plantsReact) where the plants are pressed and burnt
  Wakes wakes;            // the foam behind what moves on the water
  HullWash hullWash;      // [hulls] the waves meeting the ships' hulls on the lifted sea (hull_wash.h)
  // [hulls] (ListHull's: each hull's place and clock of the frame before, its speed eased, the frame it was last listed)
  struct HullSeen {
    Vec3 place;
    double clock = -1;
    float speed = 0;
    uint32_t stamp = 0;
  };
  std::unordered_map<uint32_t, HullSeen> hullsSeen_;
  Ranges ranges;          // range rings, each group as one outline (set ranges.groups)
  PlacedMeshes placedDraw;  // what of `placed` is not an ordinary lit model
  OrderLines orders;        // the order lines while Shift is held (order_lines.h: the host sets orders.legs, marks)
  SkyLight skyLight;  // of this frame, when a time of day is set
  Shadows shadows;
  View view;       // this frame's
  Camera camera;   // this frame's

 private:
  struct PlacedProp {  // (model null: not there - its files could not be read, or it is gone)
    const Model* model;
    Mat4 world;
    int cell = -1;  // which square of the map it is listed in (propCells_); -1: none
    // (hot metal: when a made one appeared, game seconds, -1: not known; a unit's wreck; held out of its square's group
    // while it glows, hotProps_)
    double born = -1;
    bool wreck = false, hot = false;
    bool unitWreck = false;  // [groundfix] a unit's wreck (the game's DefaultWreckage): it presses the plants under it
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
  FrameCombat combatPending_;  // AddCombat's, not yet due
  FrameCombat combat_;         // those due in the frame being added (AddFrame), for the looks that answer them
  void TakeDueCombat(double shown);
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
  // [scars] the props whose changes were applied in the last seconds (entity id -> when), and whether an entity is a prop
  // (standing, made in the game, or changed or gone just now or about to be: Effects::isProp)
  std::unordered_map<uint32_t, double> recentProps_;
  double recentPruned_ = 0;
  bool IsProp(uint32_t id) const;
  // (volume explosions) Whether harm to an area is a blast's volume: not a flak burst high in the air, a torpedo's or a
  // depth charge's under the water, a warp-in's clearing of its spot or a fire's damage over time (AddFrame)
  bool AreaMakesVolume(const FrameHit& hit) const;
  // hot metal: on this frame (and the frame before); the made props held out of their groups while they glow; the
  // tint WreckHeat hands out; the bounding sphere of a placed model
  bool hotOn_ = false, hotBefore_ = false;
  std::vector<uint32_t> hotProps_;
  Vec3 heatTint_;
  void HoldHot(uint32_t id, PlacedProp* prop);
  void UpdateHotProps();
  static void Sphere(const Model* model, const Mat4& world, Vec3* centre, float* radius);
  bool UnderWater(Vec3 centre, float radius) const;
  // the last hits on each unit's personal shield (at most eight, the newest last): where, on the model at
  // rest (world units), and when
  struct ShieldHit {
    Vec3 at;
    double clock = 0;
  };
  std::unordered_map<uint32_t, std::vector<ShieldHit>> shieldHits_;
  std::vector<Vec4> shellHits_;  // (one unit's for its shell, made anew for each)
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
  bool debrisWarm_ = false;  // [groundfix] (the debris on: each kind's glow read ahead as its model is made, Debris::Warm)
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
    float odometer = 0;  // [wakes] how far it has gone in all (the remade wake's foam keeps its place on the water by it)
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
  int rememberedFor_ = -1;  // the focus army remembered_ is of (another army has not seen those buildings)
  uint32_t frameStamp_ = 0;
  // The game's bracket pictures, each cut into its four corners: [0] a selected unit's, [1..3] the
  // kinds of `hoverKind`; corners in the picture's order: top left, top right, bottom left, bottom right.
  gfx::Texture brackets_[4][4];
  bool haveBrackets_ = false;
  void LoadBrackets();
  void AddBrackets(int kind, Vec3 position, const Quat& orientation, const StrategicIcons::Kind& unit, const Model* model);
  int lampUnits_ = 0, lampUnitsBefore_ = 0;  // units whose lamps were lit this frame, and the frame before
  std::unordered_map<uint64_t, uint32_t> lampSquares_;  // lamps lit this frame per small square of ground: beams, others
  // [nightlight] (steadyLamps) the lamps of this frame as AddLamps placed them, lit by FlushLamps once all are in; each
  // keyed lamp's level (0 off .. 1 on) as it fades, and the frame's clock it was last moved on at
  struct PendingLamp {
    Vec3 at, color, along, middle, origin;  // (color: the lamp's tint times how strongly it is on, before its level)
    float radius = 0, cone = 0, glow = 0;
    float shaftThick = 0, shaftWide = 0, shaftReach = 0, shaftAir = 0;
    float weight = 1;      // its share by the reach round the camera and the most a unit lights (0..1, smooth)
    uint64_t square = 0;   // the small square of ground it lies in
    uint64_t key = 0;      // the unit's number and the lamp's place in its list (0: none, no fade)
    uint32_t unit = 0;     // which call of AddLamps it came from (a unit's own lamps do not cut each other)
    uint8_t order = 0;     // the crowded squares' order: 2 a building's, 1 a unit's lit the frame before, 0 others
    bool beam = false;
  };
  std::vector<PendingLamp> pendingLamps_;
  uint32_t pendingUnits_ = 0;
  struct LampLevel {
    float level = 0;
    uint32_t seen = 0;  // (frameStamp_ it was last asked for)
  };
  std::unordered_map<uint64_t, LampLevel> lampLevels_;
  double lampClock_ = -1;
  uint32_t lampStamp_ = 0;
  float lampUnitsSmooth_ = 0;  // lampUnitsBefore_ eased over frames (the most a unit lights, without steps)
  void FlushLamps();
  // MOHO64_DEBUG_NIGHTLIGHT=1: every 60 frames, the lamps that popped (their light changed by more than a quarter of it
  // from one frame to the next), the lamps' shadow maps handed to another, the lights left out of crowded squares
  const bool debugNight_ = std::getenv("MOHO64_DEBUG_NIGHTLIGHT") != nullptr;
  std::unordered_map<uint64_t, float> nightShown_, nightShownBefore_;
  int nightFrames_ = 0, nightPops_ = 0, nightSwaps_ = 0, nightLeftOut_ = 0, nightLamps_ = 0, nightDiscs_ = 0;
  void NightNote(uint64_t key, float shown) {
    if (debugNight_ && key) nightShown_[key] += shown;
  }
  // MOHO64_DEBUG_LAMPS=<part of a bone's name> (or 1 for every bone): world.cpp, AddLamps
  const char* debugLamps_ = std::getenv("MOHO64_DEBUG_LAMPS");
  int pauseMark_ = -2;  // the game's pause picture among the symbols (-2: not asked for yet)
  int debugLampsTold_ = 0;
  float groundMiddle_ = 0;  // halfway between the map's lowest and highest ground
  float mistFloor_ = 0, mistDepth_ = 3;  // where the mist lies: its floor's height, and over what height it thins to 1/e
  double clock_ = 0;
  // the temporal upscaler (temporalUpscale): this frame's, whether the frame before had it, its shaders
  // (0 not made yet, 1 made, -1 they cannot be), the jitter's count, a cut, the camera's eye the frame before,
  // and the time between frames
  bool temporalNow_ = false, temporalBefore_ = false, temporalCut_ = false;
  int fsr3State_ = 0, temporalFrame_ = 0;
  // frame generation (frameGeneration): its passes (0 not made yet, 1 made, -1 they cannot be), the device asked
  // for it (MakeBetween called at each frame's end), on for this frame, on the frame before, this frame a cut, and
  // what the picture made between needs, kept from this frame's Render (ready: kept this frame)
  int framegenState_ = 0;
  bool framegenAsked_ = false, framegenNow_ = false, framegenBefore_ = false, framegenCut_ = false, framegenReady_ = false;
  FrameGeneration::Inputs framegenIn_;
  bool MakeBetween();
  Vec3 lastEye_{};
  // (airfix) the moment and the weather of the window's last frame (the hour, -1 without a time of day; the clouds, rain,
  // snow, fog, wet, mist), for the weather cut
  float lastWeather_[7] = {};
  bool lastWeatherKnown_ = false;
  double lastFrameAt_ = -1;
  float frameSeconds_ = 1 / 60.0f;
  bool raysTried_ = false, raysGround_ = false;  // (RaysReady: its shaders tried; the open map's ground in it)
  bool bounceTried_ = false, bounceMade_ = false, bounceNow_ = false;  // (bounce light: tried, made, on this frame)
  bool grassTried_ = false, grassMade_ = false, grassNow_ = false;  // (ground cover: its shader tried, made, on this frame)
  bool litSmokeNow_ = false, smokeNow_ = false;  // (lit smoke this frame; the smoke's shadow map drawn this frame)
  bool volumesOn_ = false, volumesDrawn_ = false;  // (volume explosions: on this frame; volumes drawn this frame)
  std::vector<Particles::Hot> hot_;  // (lit smoke: the lasting fires, this frame)
  bool deepTried_ = false, deepNow_ = false;  // (deep tracks: their field tried; on this frame)
  bool buildNow_ = false;  // (buildfx: the remastered build look this frame)
  bool exhaustsNow_ = false;  // (unitsfx: the real exhausts this frame)
  void PressUnit(const FrameUnit& u, const Frame& frame, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation);
  bool wildlifeNow_ = false;  // (wildlife: something of it drawn this frame)
  // (the ground cover's plants: the map's cover read; where its trees stand; where the plants grow, Terrain's)
  bool plantsReady_ = false;
  gfx::Texture treeMask_, coverA_, coverB_;
  // [snow ground] the props as the snow's shelter sees them (snow_ground.h: made the first time snow lies with snowGround on)
  gfx::Texture snowProps_;
  bool snowPropsMade_ = false;
  gfx::Texture SnowProps();
  // [snowhitch] What the snow wants made ahead of its first flake, while the game's weather can snow (SnowAhead: snow falling
  // or lying, or the weather changing by itself in a climate whose spells bring snow): PrepareSnow, from PrepareAhead. The
  // props' raster worked out on a thread of its own (the same texels: SnowPropsTexels) and made into snowProps_ when done.
  bool SnowAhead() const;
  void PrepareSnow();
  std::vector<SnowProp> SnowPropList(int* trees) const;
  std::future<std::vector<uint16_t>> snowPropsAhead_;
  std::chrono::steady_clock::time_point snowPropsStart_;
  int snowPropsTrees_ = 0, snowPropsCount_ = 0;
  void TakeSnowPropsAhead(bool wait);
  bool reflectNow_ = false;  // (reflections by ray tracing this frame)
  bool lensSnap_ = false;    // (cinecam: the camera jumped this frame: the lens's focus at once)
  bool blastsNow_ = false, grassBlasts_ = false;  // (blasts' waves or dust under way this frame; their field over the grass)

  bool outlinesNow_ = false;  // (hidden outlines this frame)
  bool lightRaysNow_ = false;  // (the small lights' shadows by rays this frame)
  bool dustNow_ = false;  // (movement dust: on this frame)
  // (photomode) the game's weather moved on to a frame; a moment of the picture's own put in for a frame (Begin) and the
  // game's own hour and weather, kept aside meanwhile, put back (the end of Render)
  float AdvanceWeather(float time, double clock);
  void PutMomentIn(float time, double clock);
  void TakeMomentOut();
  struct GameWeather {
    float timeOfDay, cloudCover, rain, snow, fog, wet, snowLying;
    bool weatherAuto;
  };
  GameWeather gameWeather_{};
  bool momentShown_ = false;
  bool surfNow_ = false;  // ([surf] surf and spray: on this frame)
  bool plantsNow_ = false;  // [plants] (plants that react: on this frame, and ready)
  std::vector<Particles::Hot> plantFires_;  // [plants] (the lasting fires near the camera, this frame)
  void PreparePlants();     // [plants]
  void PrepareAhead();      // [hitches] (readAhead)
  bool effectsAhead_ = false;  // [spikes] (the effects' pictures asked for ahead: SetLooks, once a game)
  // [spikes] (SetLooks, MakeAhead) what is made ahead is asked for half a second into the game (kAheadAfterFrames frames after
  // the simulation's looks came), not during its first frames; until then it waits here
  static constexpr int kAheadAfterFrames = 30;
  int aheadFrames_ = kAheadAfterFrames;
  std::vector<std::string> aheadPads_, aheadKinds_, aheadPictures_;
  bool AheadBegun();
  // Room on the card for what is made ahead (it holds 1-2.3 GB more by minute ten): asked of the card every 60 frames
  // (gfx MemoryBudget); while the card's use is within a gigabyte of 85 % of what the system grants this program, nothing
  // more is asked for ahead, and what would have been is made at its first use, as before. True where the card does not say.
  bool aheadRoom_ = true;
  int aheadRoomFrames_ = 0;
  void SmoothProps(double clock);  // [plants] (props the game moves beat by beat, drawn between the beats)
  struct PropBeat {
    uint32_t tick = 0xffffffffu;  // (the beat it is drawn toward)
    Mat4 at = Mat4::Identity();   // (where the beat before left it)
  };
  std::unordered_map<int64_t, PropBeat> propBeats_;  // [plants] (by its index in the map's list, or -1 - its id)
 public:
  EnergyShields energy;  // [energy shields] (last: the other members keep their places)
  // (photolook) A photographic look (photo_look.h; the visuals session's proposal 6, Chris 2026-10-18): the eye adapts
  // to the scene's brightness, a film-like colour grade (named: film, warm, coldwar, plain) and lens touches (vignette,
  // grain, colour fringe), at the end of the finishing in place of the plain tone curve. Modern look; off by default.
  PhotoLook photoLook;
  PhotoFinish photoFinish;
  // [horizon] The weather on the horizon (horizon.h; Chris, 2026-10-18, "beyond the map's edge, part 2"): on the sea
  // beyond the map's edge, winter ice while snow lies (floes thickening into pack ice, a few icebergs far out), low
  // fog banks drifting, showers (rain curtains, snow squalls) passing far out, and the clouds' shadows carried over the
  // far sea. Modern look, with the world beyond the edge (worldBeyond) and a time of day; off by default.
  bool horizonWeather = false;
  Horizon horizon;
  BeyondLand distantLand;  // (beyondland) the land far off and the soft edge's look (beyondLand)

  // [seasons] The season the map is drawn in (seasons.h; Chris, 2026-10-18: a player's option): taken in when a map is
  // opened (Open: its ground's materials, its ground cover and its props, where the map's remap.json lists them), so a
  // change shows from the next map. The picture's alone: the simulation keeps the map's blueprints. In autumn the
  // weather that changes by itself (weatherAuto) draws from the autumn climate where `climate` is Temperate (weather.h).
  // Summer, as at first: as always.
  Season season = Season::Summer;
  Season SeasonShown() const { return seasonOpen_; }  // the season the open map took
  // [seasons] Leaves in the autumn wind (seasons.h, FallingLeaves): in autumn on a map with an autumn look, the modern
  // look: drifting off its broadleaf trees (those the season draws in its own look, and those named so), and blown over
  // the ground where its cover grows. On (with the season); false: none.
  bool fallingLeaves = true;
  FallingLeaves leaves;
  // [sunlight] Sunlight through gaps (sunlight.h; the visuals session's proposals 1 and 2, Chris 2026-10-18). `lightShafts`:
  // shafts of sunlight through gaps in the woods' crowns in the low haze over the ground (strongest on a misty morning and
  // after rain), and rays under breaks in the clouds; needs the light in the air (volumetricFog). `driftingShadows`: the
  // clouds broken into heaps of many sizes, their shadows drifting over land and sea with the wind, readable from every
  // height of the camera, with or without the volumetric clouds. Modern look with a time of day; off by default (in the
  // game's "Atmosphere" setting).
  bool lightShafts = false, driftingShadows = false;
  Sunlight sunlight;
  // [stream] Streams as real water (stream_water.h; Chris, 2026-10-19): a map's stream props (the visuals track's ribbons
  // laid over the ground) drawn as water instead of as models: deeper and darker in the middle with the bed showing at
  // the edges, a soft wet shore, ripples and foam carried downstream (faster where it drops, white round rocks and
  // behind them), the sky mirrored, the sun's glint, rings in rain; their white water flowing. Modern look; off by
  // default here, in the game's "Weather Effects" (world_picture.h).
  bool streamWater = false;
  StreamWater streams;

 private:
  bool streamNow_ = false;  // [stream] (streams drawn as water this frame)
  bool horizonNow_ = false;  // [horizon] (something of it this frame)
  Season seasonOpen_ = Season::Summer;  // [seasons]
  bool leavesNow_ = false;              // [seasons] (falling leaves drawn this frame)
  std::vector<Tarmacs::Pad> padsNear_;  // [tarmac] (the remastered pads, for the ground cover cleared under them)
};

}  // namespace render
