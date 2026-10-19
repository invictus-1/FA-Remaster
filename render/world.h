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

#include "air.h"
#include "blasts.h"
#include "build_effects.h"
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
#include "icons.h"
#include "lights.h"
#include "linalg.h"
#include "mesh_renderer.h"
#include "order_lines.h"
#include "models.h"
#include "particles.h"
#include "photo_look.h"  // (photolook)
#include "post.h"
#include "ray_scene.h"
#include "bounce.h"
#include "ribbons.h"
#include "scmap.h"
#include "shadows.h"
#include "smoke.h"
#include "terrain.h"
#include "textures.h"
#include "unit_wear.h"
#include "view.h"
#include "volume_blasts.h"
#include "placed.h"
#include "ranges.h"
#include "wakes.h"
#include "water.h"
#include "wildlife.h"

namespace moho {
class Vfs;
}

namespace render {

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
  // The lamps of a unit drawn this frame (a remastered unit's list of lights): lit while the light
  // of the computed sky is low, each where its bone is. AddFrame calls it for a game's units; a
  // host that adds models itself calls it beside `meshes.Add`.
  // `armyColor` (as armies' colours are given, screen values): what a unit's own glow (a light of
  // Kind 'hull') is drawn in; without it, in the list's colour.
  void AddLamps(const Model* model, Vec3 position, const Quat& orientation, const Pose* pose = nullptr, const Vec3* armyColor = nullptr);
  // A unit's outline where it is hidden (hiddenOutlines, hidden_outlines.h), right after its `meshes.Add`. AddFrame
  // calls it for the game's units the player may see; a host that adds units itself calls it beside `meshes.Add`.
  // `color`: its strategic icon's; `building`: a structure (it hides the units behind it); `kind`: its strategic
  // icon (null: none), which the outline gives way to as it comes in with the distance.
  void MarkOutline(const Model* model, Vec3 position, Vec3 color, bool building, uint32_t id, const StrategicIcons::Kind* kind);
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
  // the waves have wetted it, and the bright net of light on a shallow bed sharper and stronger. Modern look, a map
  // with water; off by default.
  bool waterDetail = false;
  // Ripples on the water (ripples.h; water.AddRipple): shells landing in the water, ships, hover units and units
  // wading leave rings and wakes that spread, cross each other and die out. Modern look, a map with water; off by
  // default.
  bool waterRipples = false;
  // Winter water (winter_water.h): while snow lies (snowLying) the water goes over to a cold palette (deep water a dark
  // slate, the shallows a muted grey-green, the bed dimmer and colder), slush and thin ice gather along the shores, the
  // foam greys; and no snow lies under the water. It follows snowLying, so a thaw brings the summer water back. With no
  // snow lying, nothing changes. Modern look with a time of day, a map with water; off by default (in the game's
  // "Weather Effects").
  bool winterWater = false;
  // Ground cover near the camera (grass.h): grass blades on grassy ground, coloured from it, swaying in the wind,
  // low and gone with the distance (nothing when the camera is far). Modern look; off by default.
  bool groundCover = false;
  // Battle scars (decals.h, Decals::battleScars): the scorches and craters of the fighting stay on the ground,
  // weathering, up to a most for cost. Off by default (the game's own times).
  bool battleScars = false;
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
  // [energy shields] Shields that look like energy (energy_shields.h; docs/render-lighting.md, item 11): a shield's
  // bubble bends what is behind it, rings run across it from each hit, its pattern flares where it is hit and as it
  // weakens, and its light falls on what is inside it. Drawn by `energy` (at the end of the class) in place of the
  // plain bubble (not for the effects stand's original side). Modern look; off by default.
  bool energyShields = false;
  // Battle scars on units (unit_wear.h): each unit keeps marks of the hits it survived, of the kind of what hit it,
  // in three tiers by all the damage it has taken, mended (not gone) as it is repaired. Modern look; off by default.
  // Snow on units (unit_wear.h): each unit holds its own share of the lying snow, which builds up while it stands
  // in snowfall and comes off as it moves, fires, is hit, burns or a blast is near. Modern look; off by default.
  bool unitScars = false, unitSnow = false;
  UnitWear wear;
  // A game's beat for them: AddFrame calls it; a host that draws units itself (the effects stand) calls it with its
  // frame first. Only the first call for a beat counts.
  void WatchWear(const Frame& frame);
  // What a unit (its entity id) wears this frame, for MeshRenderer::Wear; null: nothing of its own.
  const WearLook* WearOf(uint32_t id) {
    wear.scars = unitScars;
    wear.snow = unitSnow;
    return look == Look::Modern && (unitScars || unitSnow) ? wear.Look(id, clock_) : nullptr;
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
  // Motion (motion.h): where each pixel was the frame before, for the temporal upscaler; it needs the
  // scene drawn with one sample a pixel (gSceneSamples 1). `motionView`: a view for checking it (1 the
  // motion as colours, 2 the frame before carried to this one; Motion::Check; 5 the temporal upscaler's
  // reactive mask, Post::Reactive, in red).
  bool motionOn = false;
  int motionView = 0;
  Motion motion;
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
  // A frame drawn for something else between two of the game's (the minimap's ground): no temporal upscaler,
  // no motion, no light in the air and no volumetric clouds, and what they keep of the frames before is left
  // for the game's next frame.
  bool sidePicture = false;
  // (cinecam) A frame drawn for a photo (WorldPicture::TakePhoto, the viewer's --photo): the world as the game's
  // frame draws it, at the photo's size and the scene's own (no FSR), but without what keeps frames before for the
  // game's next (the temporal upscaler, motion, frame generation's picture), and with nothing of the interface laid
  // over it (icons, bars, order lines). What else of the interface is in the world the host takes out itself.
  bool photo = false;
  // (photomode) A frame drawn for a tile of a photo (WorldPicture::StartPhoto, photo.h): the photo's own camera, round
  // which what is placed for the camera is placed in every tile of it (the sun's and the lamps' shadows, the grass, the
  // rain's box, the bounce light's probes, the smoke's shadow, lightning, deep tracks, the hot things' lights), so that
  // the tiles agree where they meet. Off at first.
  bool photoTiled = false;
  Camera photoWhole;
  const Camera& Placing() const { return photoTiled ? photoWhole : camera; }
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
  Wakes wakes;            // the foam behind what moves on the water
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
  void PressUnit(const FrameUnit& u, const Frame& frame, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation);
  bool wildlifeNow_ = false;  // (wildlife: something of it drawn this frame)
  // (the ground cover's plants: the map's cover read; where its trees stand; where the plants grow, Terrain's)
  bool plantsReady_ = false;
  gfx::Texture treeMask_, coverA_, coverB_;
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
 public:
  EnergyShields energy;  // [energy shields] (last: the other members keep their places)
  // (photolook) A photographic look (photo_look.h; the visuals session's proposal 6, Chris 2026-10-18): the eye adapts
  // to the scene's brightness, a film-like colour grade (named: film, warm, coldwar, plain) and lens touches (vignette,
  // grain, colour fringe), at the end of the finishing in place of the plain tone curve. Modern look; off by default.
  PhotoLook photoLook;
  PhotoFinish photoFinish;
};

}  // namespace render
