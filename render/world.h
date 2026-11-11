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
#include "effect_files.h"  // [shieldhit] (UnitFaction)
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
#include "rock_hits.h"  // [rockhits]
#include "scmap.h"
#include "seasons.h"  // [seasons]
#include "snow_ground.h"  // [snowhitch] (SnowProp)
#include "shadows.h"
#include "smoke.h"
#include "stream_water.h"  // [stream]
#include "sunlight.h"  // [sunlight]
#include "sun_mask.h"  // [shadowmask]
#include "surround.h"  // (surround)
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
#include "fx_mirror.h"  // [fxmirror]
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
  // [restart] (the interface's request 45 R1: a replay's jump back starts a new session on the map already open, where
  // opening it again took 2-3 s on a 4v4) Everything a game added let go of, the map as Open left it: no unit's traces, no
  // wreckage, scars, tracks, game's decals, effects or particles, no hits not yet shown, debris, dust, unit wear, hot metal or
  // ripples; the map's props as the map has them; the ground the simulation laid flat as it was (drawn so from the next
  // Begin); the clock, the weather's doings on surfaces (wet, snowLying) and what was kept of the frames before begun again;
  // the interface's lists empty. Kept: the ground, the textures, the pipelines, the models already made, the map's own
  // decals, props and looping effects, the sky and every setting. The host then gives the new game's frames from its start.
  // Between frames (not between a Begin and its Render). Nothing without a map.
  void RestartGame();
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
  // [debrispile] Whether a unit's wreck is drawn as its baked pile (its kind has a baked set: baked_debris.h; the debris on).
  bool PileKind(const std::string& unitId);
  // Deep tracks (deepTracks): a frame's units press the ground as AddFrame has them do, for a host that draws them
  // itself (the effects test stand).
  void PressGround(const Frame& frame);
  // [dents] A shell's hole where a shot struck the ground (deep tracks: the picture only; the ground's heights the game
  // and the picking go by stay as they are): `at` where, `size` how large across, as the scorch the game lays for it
  // (Blasts::FromImpact's: a small gun's 0.2, an artillery shell's 1 to 3). None in the air or on or under the water.
  // AddFrame makes them from the simulation's impacts on the terrain (else from the blasts on the ground); the effects
  // stand from its shot's (--fx-dent).
  void DentGround(Vec3 at, float size);
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
  // The unit under a point of the screen: UnitsAt's first within 10 pixels ([pick] the original's way, by the unit's box and
  // nearest the point on the screen); 0xffffffff: none.
  uint32_t UnitAt(float x, float y) const;
  // The units whose middle lies in a rectangle of the screen.
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const;
  // [pick] (ui request 43) Every unit under a point of the screen as the original's pick finds them (CWldSession::UpdateSelection
  // 0x86f520): its selection box (the model's box, the top share its blueprint picks, scaled by its blueprint) shown within
  // `tolerance` pixels of the point (the original's ui_SelectTolerance; FAF 10), or, far off, its icon's middle within that
  // and 10 pixels; the nearest to the point on the screen first (by where the unit stands), each once. Units whose whole
  // mesh is hidden are not found. The interface chooses among them by its own rules (carriers, parents, armies).
  void UnitsAt(float x, float y, float tolerance, std::vector<uint32_t>* out) const;

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
  // [glitches] How a ghost is drawn (Chris's big test, ui's request 41: "when queued it shouldn't show the finished structure
  // only a green outline like a ghost of it"): 1 see-through, as the original's UnitPlace (MeshRenderer::SeeThroughGhost:
  // its tint lit by its shape, the ground showing through, its outline brighter); 0 the solid model in one colour, its
  // textures' light and dark kept, as before. A player's option.
  int ghostLook = 1;
  // [detail] (Chris, 2026-11-06: close up, a factory's side "feels too low res"; with the visuals track's 4K re-bakes) A
  // remastered unit's surface gains a fine grain of its own where its textures are magnified (a texel over more than about
  // two pixels: only at the nearest zoom), in the paint's light and dark and in its relief, fading in with the magnification,
  // none at play zoom. 0: none (as before); 1 the default; the viewer's --unit-detail.
  float unitDetail = 1.0f;
  // [stealthlook] (Chris's big test: "Stealth on and off works though we have no effect for when a unit is stealthed ... in game
  // cybran has all units look a specific way") cloakLook: a cloaked unit (its script set FAF's cloak mesh, '<mesh>_cloak', the
  // shield shader of its faction: Blueprints.lua ExtractCloakMeshBlueprint) drawn as the original draws it, its body a
  // see-through shimmer in its faction's shield colour, no lamps; false: as an ordinary unit, as before. stealthLook (ours: the
  // original shows nothing for radar stealth): the player's own and allied units hidden from radar (their stealth on, or in an
  // allied stealth field) drawn as ever, in a faint shimmering veil, so the player sees which are. Players' options.
  bool cloakLook = true, stealthLook = true;
  struct StealthField {  // [stealthlook] (a radar stealth field of the player's side, this frame)
    float x, z, radius;
  };
  std::vector<StealthField> stealthFields_;
  int cloakedNow_ = 0;  // [stealthlook] cloaked units listed this frame
  // [glitches] The interface's wide lines (`width` over 1.25 pixels: a building's site outline, the guide while it is placed)
  // drawn this many world units nearer the eye, where they show on the screen the same, so that they lie over the ground
  // cover rather than under its plants ("Green building outline when trying to place structure doesn't show so its hard to
  // line up"), with a dark edge a pixel wide either side so that their green reads on green ground. 0: as before (in the
  // ground cover, no edge).
  float guidePull = 1.2f;
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
  // [lightshade] The ground shadows the larger small lights (Lights::shade, lights.h: the review of 2026-11-09, light 11):
  // an explosion's flash, a nuke's or a big glow behind a ridge no longer lights the ground, the units, the smoke and the
  // mist on the ridge's near side. By the map's heights alone (a table per light, no rays): hills and ridges, with and
  // without ray tracing; hulls and buildings only by the rays (rayLightShadows), as before. Modern look. Off: as before.
  bool lightShade = true;
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
  // [shadowstep] The sun's shadow cascades' sizes from a ladder as the camera zooms, each kept while it still serves
  // (shadows.h, Shadows::zoomSteps): the shadows stand still while zooming, but for a step now and then. On by default;
  // viewer --shadow-zoom-smooth: off (as wide as the zoom asks every frame, as before).
  bool shadowZoomSteps = true;
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
  // (surround) With the world beyond the edge, on a map that brings a sculpted surround (x_surround.json beside its
  // x.scmap; surround.h): that land is drawn past the map's edges, not the map laid out again mirrored. Read the first
  // time it is wanted. Modern look. Chris, 2026-11-10: on by default (a map that brings no surround draws exactly as
  // before: the map laid out again mirrored); false: that for every map (viewer --no-surround).
  bool mapSurround = true;
  // (surround) The detail look (surround.h; the visuals session's detail pass of 2026-11-10), where the map's surround
  // brings its ground's masks, its forest and its scenery; all of it only with mapSurround:
  //   surroundBlockOut: its ground as the first look had it (the map's colours by height and slope), although it brings
  //     masks (viewer --surround-blockout);
  //   surroundScenery: the scenery its json names (its "decor": plants, saplings, rocks and tree groups in the map's own
  //     decor format, scmap.h LoadDecor) stands on it, drawn by the models and shaders the map's own props are drawn by
  //     (each by the season's blueprint, with their levels of detail, the wind, the snow and the light as the map's).
  //     A picture only, and kept apart from the map's props: nothing of the game finds them (no fire, no felling, no
  //     clearing under structures, no scars), they are in no shadow map and not in the ray tracing scene
  //     (MeshRenderer::ListGroupApart). Read the first time it is wanted. False (viewer --no-surround-scenery): bare ground,
  //     its forest drawn as one canopy up to the map's edge;
  //   surroundSceneryPixels: how many times the smallest size on screen of the map's own props (MeshRenderer::minPixels)
  //     a piece of that scenery must have to be drawn at all: it stands where no one plays, past ground that dims with
  //     the distance (viewer --surround-scenery-pixels);
  //   surroundForestFloor: the layer of the map's ground laid under the surround's forest where its json does not name
  //     one ("forest_floor"); -1: none (viewer --surround-forest-floor).
  //   surroundSceneryFrom, surroundSceneryTo: from how far from the eye that scenery is thinned (a third of each square's
  //     pieces left out, then two thirds half-way) to how far none of it is drawn (a square by its nearest point): from
  //     the game's farthest zoom the surround's forest is the ground's canopy alone, and the ground goes over to the
  //     canopy where the trees are left out (viewer --surround-scenery-reach <from>,<to>).
  bool surroundBlockOut = false;
  bool surroundScenery = true;
  float surroundSceneryPixels = 2.0f;
  // (and its levels of detail chosen as if it stood this many times as far: viewer --surround-scenery-lod)
  float surroundSceneryLod = 1.5f;
  float surroundSceneryFrom = 500, surroundSceneryTo = 1100;
  int surroundForestFloor = -1;
  // [edgeband] (Chris, 2026-11-10, shown the game's picture over the edge with the surround on, the edge not to be
  // found: "Faint dimming".) The land past the edge is a quarter dimmer and a little greyer (lighting.h BeyondFar, as
  // all that lies beyond the edge), but over the soft edge's 60 world units, which on ground this alike told nobody
  // where units cannot go. With a sculpted surround the dimming comes in over this many world units instead (12: a
  // soft step that reads from the game's height and is no line; it lies on the ground, so it does not shift with
  // the zoom). 0: the soft edge's own band, as before (viewer --surround-edge-band 0).
  float surroundEdgeBand = 12;
  // [snowsur] The surround under lying snow (Chris, 2026-11-10: from far up the land past the edge lay under a flat
  // grey snow beside the map's own white, and the far sea's floes stood as hard-edged cells right beside the map):
  //   - its ground holds the snow as the map's ground does (surround.h: the ground's own snow rule, materials and
  //     shading, Terrain::BindSnow), where the ground lays its snow by its own pipeline (snowGround);
  //   - surroundIceFrom: beside a sculpted surround the horizon's floes (horizon.h) begin this share of the surround's
  //     reach out, not a few units past the edge: the sea beside the map is open as the map's own winter sea, and the
  //     floes lie where the haze has taken most of the land (0: as before; viewer --surround-ice-from).
  float surroundIceFrom = 0.55f;
  int surroundPropsListed = 0;  // (of the frame last begun: pieces of the surround's scenery in the squares in view)
  size_t SurroundProps() const { return surroundProps_.size(); }
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
  // [hazeamt] (the interface's request 50: a player's "Haze & Mist" amount) How much of the clear weather's haze and mist
  // the modern look shows, with a time of day: 1 as made, 0 clear air, up to 2. It scales the mist on low ground (the
  // hour's and the wet ground's), a cinematic camera's least haze, the air's own dust (below 1 the lamps' and small
  // lights' strengthening on it goes up as it goes down, so their beams stay), the light shafts' band's own haze and the
  // low haze under broken clouds, the haze the land far off is lost in (at 0 it reaches 2.9 times as far, at 2 0.6
  // times; never none: the land's end is hidden in it) and the horizon's standing fog banks. It leaves what the weather
  // itself brings (fog, a storm's and a snowfall's murk, the low fog's layer): the weather is the game's own choice.
  // The sky's own horizon haze and the clouds are the sky's. At 1 every number is what it was.
  float hazeMist = 1;
  // [weather] Fog as a low layer (World::Begin; Chris's snapshot test, 2026-10-25): fog past a quarter lies on the ground and
  // the water in the mist's layer, deeper than the mist, and only a little of it thickens the air all over. Off (the
  // viewer's --fog-old): all of it in the air all over, as before.
  bool lowFog = true;
  // [mistpatch] With the low fog (Chris's second test, 2026-11-04: "Fog has gotten better but its still too grey it should be
  // more like a mist like not every single inch of the map needs it"): in a fog the mist's layer (the fog's and a morning's
  // mist with it, as much as the fog is thick) lies in wide patches with clear air between them (view.h, MistPatches; `mistClear`: where their field begins to hold it, 0.5: some
  // two fifths of the ground in a patch, more: fewer and smaller), and of the quarter of it the air all over took and of
  // what the light shafts' hazes took, a thick fog leaves only a little. Off (the viewer's --fog-even): the fog of
  // 2026-11-03.
  bool mistPatches = true;
  float mistClear = 0.5f;
  // [snowshade] (Chris, 2026-11-04: "in snowy conditions I feel like the snow lacks shadows", and "everything else is good its
  // just shadows on the snow a bit") Under a snowfall's overcast the clouds' shadow takes four fifths of the sun wherever a
  // heap of them lies (sunlight.h), so that the light on snow there was nearly all the sky's even grey light, and the white
  // snow's bounce lifted what shade there was: slopes toward and away from the sun alike, the trees' and the rocks' shadows
  // all but gone. With this (the modern look, a time of day, by day; as much as the sky is overcast, from half covered to
  // some nine tenths, and on each surface as much as snow lies on it):
  //  - a weak sun through the clouds: at least `snowShadeTune[0]` of its light reaches the snow (where the clouds' shadow
  //    took more), shadowed by what stands toward it, and the sky's light there cut by `[6]` of what that brings to open
  //    level snow (1: open level snow in the sun as bright as before, only the shade deeper; less: the sun's share partly
  //    added, so that a face turned away from the sun does not go as much darker);
  //  - the sky's light brighter on what faces the sun's side and darker on what faces away (`[1]`: a vertical face's share);
  //  - the sky's light in the shade bluer (`[2]`: 1 some 9 % less red and 19 % more blue, the same brightness);
  //  - the snow's cut of the sky's light deeper (`[3]`: its power 1 + that), the shelter's (valleys, beside what stands round
  //    it) with the hollows and folds of the ground under it (`[4]`: up to six tenths of that taken in a fold whose sides
  //    rise a fifth of `[5]` world units over it; read on rings of `[5]` and two and a half times that).
  // Measured on SETONS_R's west hill under a snowfall from Chris's camera (_scratch/render/snowshade/REPORT.md): the snow's
  // luminance spread (RMS contrast) half as much again, the new shade some 4 % bluer, the face (turned from the sun) 14 %
  // darker, open level snow as bright as it was; under the woods' overcast the trees' shadows show softly. Off (the viewer's
  // --snow-shade 0): the snow lit as before, every picture the same.
  bool snowShade = true;
  float snowShadeTune[7] = {0.32f, 0.15f, 0.7f, 0.2f, 0.3f, 6.0f, 0.65f};
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
  Machinery machinery{&particles, &ribbons, &build};  // [machinery] (factories' parts at work: machinery.h)
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
  // [fallsize] Rain's and snow's widths in the window's pixels when an upscaler brings the scene up (FSR 1 or FSR 3 at a
  // share under 1; weather.h, Precipitation::windowShare): a flake as large on the screen as without the upscaler. False:
  // in the smaller picture's pixels (1.7 times as large on the screen at 59 %), as before.
  bool fallWindowSize = true;
  // [fallpace] The fall as fast on the screen at every zoom, and a flake's sway by its own box on the ladder (weather.h,
  // Precipitation::steadyPace). False: one speed in the world and the sway by the zoom, as before.
  bool fallSteadyPace = true;
  // [fallhigh] (Chris, 2026-11-10) the falling rain and snow thin out to nothing as the camera rises past where their
  // box can still fill the view (Precipitation::highFade, 560 to 1000 world units away); the snow lying and the
  // clouds stay. False: from the farthest zoom the fall fills a square in the middle of the picture, as before.
  bool fallFadeHigh = true;
  // [falltrail] With FSR 3, what falls marked in its transparency and composition mask too (Fsr3::Inputs::composition): no
  // lock on a flake, so no trail of its light where it was. False: the reactive mask alone, as before.
  bool fallNoLocks = true;
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
  // [fxmirror] The effects' mirror image in the water (fx_mirror.h; the review of 2026-11-09, water 9): what is drawn after
  // the water - the particles' fire, flashes and smoke, beams, tracers and trails, the volume blasts, the energy shields'
  // bubbles - was in none of its mirror (the sky, and what the picture showed before the water), so that a night fight at
  // sea showed fire over water that mirrored none of it. With it they are drawn once more before the water, mirrored about
  // its level, into a picture of half the scene's size, which the water lays into its mirror: moved and drawn out into
  // streaks by the waves' tilt, under its Fresnel term and foam, hidden where a hull or a shore stands between. Modern
  // look, a map with water, the eye above it, and only while there is something to mirror (else nothing of it runs). On
  // by default; false: the water as before, to the pixel (viewer --no-fx-mirror). The streams' water takes none of it.
  bool waterFxMirror = true;
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
  // [underfoot] The map's render-only props (its decor: plants, small rocks, branches; scmap.h, LoadDecor) taken away where
  // a structure stands or is being built, and back once it is gone (ClearUnderStructure). Chris's second test (2026-11-04):
  // "foliage shouldn't be glitching into a structure. If a structure is built the foliage below should be removed". On by
  // default; viewer --keep-decor-under-structures: off (they stand on inside the buildings, as before).
  bool clearUnderStructures = true;
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
  // [rockhits] Rocks that answer fire (rock_hits.h; Chris, 2026-11-07): a shot or blast that lands on or beside one of the
  // map's rocks chips it (small pieces of its stone fly off, some stay lying at its foot for minutes) and throws a puff of
  // rock dust; each rock keeps how much it was hit, its soot darkening it in patches, cracks opening over it, a fresh
  // coat of dust fading; the soot eases back a little over minutes. To be seen only: the props stay where the game has
  // them. Modern look; off by default (proposed for the "Battle Effects" bundle). RockHit: a hit told by the host itself
  // (the effects stand's shots; a game's come from its frames: AddFrame).
  bool rockHits = false;
  RockHits rocks;
  void RockHit(Vec3 at, float size, Vec3 way = {});
  // Deep tracks (deep_tracks.h, `trackField`): where units drive and walk, the ground is pressed by the kind it is:
  // troughs and berms in snow, dark wet ruts in mud, compacted dirt, soft sand with ridged edges, grass laid flat and
  // worn to the earth; fading with the weather and the ground. The game's tread marks and footprints are pressed in
  // by their own shapes instead of laid as pictures. [dents] Shots that strike the ground leave shell holes in it
  // (DentGround), deepest in snow and mud. Modern look; off by default.
  bool deepTracks = false;
  // [churn] Shell holes one over another (deep_tracks.h, DeepTracks::churnedHoles; Chris's test of 2026-11-09): where many
  // shots strike one spot each new hole is dug into what is there - whole within its lip, its earth thrown over the
  // older holes beside it, none deeper than a hole can be - so the spot reads as one churned field of bowls; and the
  // flat marks the game lays (the shots' scorches, kept as battle scars) give way where the ground shows a hole
  // (decals.h, Decals::holes), so that none lies over a hole like a sheet of mud. False: every hole added to what is
  // there and the marks over them, as before (the middle of such a spot one flat dark sheet; the viewer's --holes-added).
  bool churnedHoles = true;
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
  // [shieldhit] Personal shields at rest (Chris, 2026-11-09, of the Dragonite's after its lattice was made finer: "just
  // needs to be off unless hit"). A unit whose script has put its personal shield's mesh on it (a commander's upgrade, an
  // Obsidian, a Harbinger, a mod's Dragonite) is drawn in the modern look with its faction's shield light along its
  // outline and its lattice over its hull for as long as the shield is up, as the original draws its shield mesh
  // always. False (as of 2026-11-10): nothing of it at rest; a hit lights the whole shield for its second - outline and
  // lattice at twice their rest strength, the cells round the hit bright, a ring running out from it - and it goes out
  // again. All personal shields drawn along the unit's outline, the game's and the mods'; a mod's own capsule shell
  // (Display.PersonalShield) showed only its hits already. True: as before. The original look keeps its glow always.
  // (The renderer chat, the same day: Chris spoke of the Dragonite, and a commander's shield upgrade is something
  // players read off the unit at rest, as in the original. So three values: 0 no personal shield shows at rest; 1 (the
  // default) the game's own units keep their rest light - ids the game and FAF give, UnitFaction - and a mod's unit
  // shows only its hits; 2 all at rest, as before. ShieldOnlyHits says which holds for a unit.)
  int personalShieldsAtRest = 1;
  bool ShieldOnlyHits(const std::string& unit) const {
    if (look != Look::Modern || personalShieldsAtRest >= 2) return false;
    return personalShieldsAtRest <= 0 || UnitFaction(unit).empty();
  }
  // [warpin] The commanders' warp-in in their faction's colours (Chris, 2026-11-09: "warp-in in faction colours"; the
  // visuals track's section 119). The modern look with the remastered effects: (1) a commander warping in - the script
  // has set its phase mesh on it (CommandUnit.lua WarpInEffectThread: Display.WarpInEffect.PhaseShieldMesh for six
  // seconds) and no shield hangs on it - is drawn as energy of its faction's colour, by the visuals track's warp-in
  // sheet of that faction (/remastered/effects/entities/PhaseShield/<faction>/warpin.dds: threads that climb, each on
  // its own beat, drifting patches), fading into its own paint over the last second and a half; it was drawn as a
  // personal shield at rest (the same mesh name: the shields' lattice, the UEF's only), and a mod's commander (its
  // shield showing only its hits, personalShieldsAtRest) with nothing at all. (2) The light flashes the warp-in's
  // script throws (UnitTeleport01_script.lua: CreateLightParticleIntel with 'ramp_blue_13') take the army's faction's
  // ramp where the visuals track has made one (/remastered/textures/particles/warpin/<faction>_<ramp>.dds), the light
  // and the glow on the ground both (Effects::warpInColours). False: both as before. The original look and the
  // original effects never have either.
  bool warpInColours = true;
  // (how long the script keeps the phase mesh on: the warp-in's look is drawn no longer, whatever a script does)
  static constexpr double kWarpInSeconds = 6.0;
  // [lampcolour] (Chris, 2026-10-28) a unit's own faint glow (its lamps of kind 'hull') in its army's colour (true, as
  // before), or in its faction's own, the colour its lamps' file gives (UEF blue whatever its army's colour)
  bool lampsTeamColour = true;
  // [hullglow] (Chris's big test of 2026-11-01: "lights being too bright at night", "a very bright red glow ... stacking on
  // top of other structures", "unit glow is a bit bright ... for battleship") a unit's own glow (its lamps of kind 'hull')
  // toned down: `hullGlow` times as strong and `hullReach` times as far as its lamps' file says (0.75 and 0.85, about half
  // the light in all; 1 and 1 as before), an army's colour as bright to the eye whatever its hue (`hullEvenColours`: a
  // yellow army's glow was four times a red one's, as light; the brightest hues lowered toward the red's), half of it in
  // the air (`hullAir`, 1 as
  // before: the fog round a base glowed in the army's colour) and, with steadyLamps, the glows that lie over each other
  // sharing (`hullShare`: where n glows add up, each 1 / n of 1 + ln(n) of them, as the build lights share, so that a
  // crowded base glows more than one building, never without bound). Viewer --hull-glow old: all as before.
  float hullGlow = 0.75f, hullReach = 0.85f, hullAir = 0.5f;
  bool hullEvenColours = true, hullShare = true;
  // [buildingglow] (Chris's second test of 2026-11-04: the glow at night "too dull now on structures, factories mainly", "a
  // glow vs actual lights on them", small units fine; "the naval factory's glow a bit too strong") with the glow toned down
  // (hullShare on), a structure's own glow and lamps apart from the units': its glow `hullBuildingGlow` times hullGlow
  // (1.33: at the slider's 0.75 as strong as before the big test, never stronger than hullGlow 1 makes it), reaching at
  // most `hullBuildingMost` world units (20: a naval factory's lamps' file gives 43, twice a land factory's, its pool over
  // the whole bay; every other structure's is within it), shared only with the other structures' glows lying over it (the
  // units round a factory, and the one just built in it, no longer dim it) and less (`hullBuildingShare`: each share taken
  // to that power, 0.5: four glows over each other 0.77 of each, not 0.6); and its other lamps (beacons, strips, windows)
  // `hullBuildingLamps` times as strong (1.3), so that they read as lights on it, not a glow round it. With hullShare off
  // (the "Unit Glow at Night" slider at its top, viewer --hull-glow old) or `hullBuildings` false (viewer --building-glow
  // old) a structure's as a unit's, as before.
  bool hullBuildings = true;
  float hullBuildingGlow = 1.33f, hullBuildingMost = 20, hullBuildingShare = 0.5f, hullBuildingLamps = 1.3f;
  // [lampbudget] (Chris's big test: "top circle is no light but at zoom a light pops", one lamp lit of a naval factory's
  // three alike) where many units are lit, the units near what the camera looks at keep all their lamps (the most a unit
  // lights, 6,000 lamps over the units in reach, given in full within a sixth of the lamps' reach round it, falling to the
  // even share by two fifths of it), so that the building one looks at does not light more of its lamps as one zooms in.
  // False: every unit in reach the same share, as before (viewer --lamp-budget even).
  bool lampsNearFirst = true;
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
  // [fgquery] (the interface's ask, 2026-11-05: its frame counter counts the real frames only) whether frame generation
  // makes a picture between this frame and the next: on, its passes made and not refused, and asked of the device
  bool FrameGenerationNow() const { return framegenAsked_; }
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
  // [glitches] The UEF's glow slab laid over a structure as its build begins (the game's UEFBuildEffect02, an AlphaFade
  // slab a twentieth wider than the structure; its script leaves it to its blueprint's six-second lifetime, in no effects
  // bag) fades out over this many seconds once no unit under construction stands under it: a structure finished (or given
  // up) within those six seconds no longer keeps a blue square over it (Chris's big test, "When building is finished too
  // quickly a blue square is left for a few seconds"). 0: it lasts its lifetime, as the original's does.
  float finishedSlabFade = 0.4f;
  // [glitches] The quantum gate's field remastered (MeshRenderer::remasteredGate), with the modern look and the remastered effects
  // (EffectsLook::Remastered); false, or the original look or effects: the original's texture, as before. A player's option.
  bool remasteredGate = true;
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
  // [cloudkeep] The frames are drawn for a run by their count (the viewer's --frames: a test, a picture, a film), not for
  // someone watching: what the clouds, the light in the air and the bounce light keep of the frames before then goes by
  // the frame as it always did, not by the clock, so that such a run gives the same picture however fast it was drawn.
  bool steadyFrames = false;
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
  // [uplod] (Chris's test, 2026-11-09: "units seem to lack a lot of details ... Up close the details are there but it seems
  // like at normal view they lose that sharpness") With an upscaler at a share under 1 (FSR 1 or FSR 3) a model's level of
  // detail is chosen by its size in the WINDOW's pixels (View::lodPixels), as the comment at the choice always said ("its
  // size on the SCREEN"): by its size in the smaller picture a unit went to its coarser level at 59 % of the distance (at
  // 59 % of a 1080p window from 50 units away the row of UEF units had 0.74 million triangles for 1.52 million, a factory
  // its far level). False: by the smaller picture's pixels, as before (fewer triangles: a speed setting's saving).
  bool upscaleLod = true;
  // [upmip] With FSR 1 at a share under 1 the textures are read log2(share) finer, as with FSR 3 (and as AMD asks for
  // FSR 1: -0.38 at 77 %, -0.58 at 67 %, -0.79 at 59 %, -1 at 50 %): without it they were read for the smaller picture
  // and then stretched, a level blurrier than the window could show. False: no bias with FSR 1, as before.
  bool upscaleMipBias = true;
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
  // [fogwar] Is that unit kind (FrameUnit::blueprint, its id) a structure: its blueprint's Physics.MotionType none (the
  // id's third letter 'b' where the blueprint cannot be read). What the fog of war remembers as it was last seen.
  bool IsStructure(uint32_t blueprint, const std::string& id);
  int RememberedCount() const { return static_cast<int>(remembered_.size()); }  // all it keeps, gone or not
  Shafts shafts;          // lamps' beams seen in the air
  Tracks tracks;          // where lying snow is trodden away by units (tracks.on switches it)
  DeepTracks trackField;  // deep tracks (deepTracks): the field of where the ground is pressed
  PlantLife plantLife;    // [plants] (plantsReact) where the plants are pressed and burnt
  Wakes wakes;            // the foam behind what moves on the water
  HullWash hullWash;      // [hulls] the waves meeting the ships' hulls on the lifted sea (hull_wash.h)
  FxMirror fxMirror;      // [fxmirror] the effects drawn mirrored for the water (fx_mirror.h; waterFxMirror)
  // [hulls] (ListHull's: each hull's place and clock of the frame before, its speed eased, the frame it was last listed)
  struct HullSeen {
    Vec3 place;
    double clock = -1;
    float speed = 0;
    uint32_t stamp = 0;
  };
  std::unordered_map<uint32_t, HullSeen> hullsSeen_;
  uint32_t hullsSwept_ = 0;  // (the frame its forgotten ones were last looked for)
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
    // [debrispile] a unit's wreck whose kind has a baked set (baked_debris.h): drawn as its pile with the debris on, else as
    // its model; on its own either way, never in its square's group (pileProps_)
    bool pile = false;
    // [fogglow] (Chris, 2026-11-08: a wreck's glow "should be hidden in fog of war. Unless you've seen that unit then it
    // stays") whether the army the picture is for ever saw the unit it is the wreck of; if not, it glows only in sight
    bool glowKnown = true;
  };
  struct Pickable {  // a unit of the frame, for UnitAt, UnitsAt and UnitsIn
    uint32_t id;
    Vec3 centre;
    float radius;
    // [pick] (ui request 43) its selection box, as the original's pick tests it: the model's box (world units, the unit's own
    // axes) cut to its top share and scaled by its blueprint's Selection* (SelectionOf), at `at` turned by `turn`
    bool boxed = false;
    Vec3 at, lo, hi;
    Quat turn;
  };
  // [pick] a unit kind's selection numbers (its blueprint's): the share of its model's height from the top that is picked
  // (SelectionMeshUseTopAmount; 1: all of it) and the box's scale along x and z (SelectionMeshScaleX / Z)
  struct Selection {
    bool known = false;
    float top = 1, scaleX = 1, scaleZ = 1;
  };
  std::vector<Selection> selections_;
  const Selection& SelectionOf(uint32_t blueprint, const std::string& id);
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  int screenWidth_ = 1, screenHeight_ = 1;  // the picture's size on the screen (the scene's is the view's)
  View screenView_;  // the view at the screen's size (icons and bars are drawn by it)
  bool haveMap_ = false;
  gfx::Texture environment_, sky_, selectionRing_;
  std::vector<PlacedProp> props_;  // the map's, by their index in the map file's list
  // [restart] (RestartGame) the map's heights and props as Open left them, the number of the last of the map's own decals
  // then (Decals::LastAdded) and how many deposits' marks there were; the heights put back, drawn so by the next Begin
  std::vector<uint16_t> heightsAtOpen_;
  std::vector<PlacedProp> propsAtOpen_;
  uint32_t decalsAtOpen_ = 0;
  size_t depositsAtOpen_ = 0;
  bool heightsRestored_ = false;
  std::unordered_map<uint32_t, PlacedProp> madeProps_;  // made during the game (wreckage), by entity id
  // [debrispile] the made props drawn as their piles (PlacedProp::pile), by entity id: their units' kinds; whether the piles
  // are drawn this frame (the debris on, the modern look: else their models); each told to the debris or drawn (ListPiles)
  std::unordered_map<uint32_t, std::string> pileProps_;
  bool pilesNow_ = false;
  void ListPiles();
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
    std::vector<uint32_t> underfoot;  // [underfoot] its decor taken out of ofMap while a structure stands over it
  };
  static constexpr float kPropCell = 32;
  std::vector<PropCell> propCells_;
  int propCellsX_ = 0, propCellsZ_ = 0;
  float propReach_ = 0;  // the largest prop's reach from its place, in world units
  int PropCellAt(const Mat4& world) const;
  void ListProp(PlacedProp* prop, uint32_t key, bool made);
  void UnlistProp(PlacedProp* prop, uint32_t key, bool made);
  // [underfoot] (clearUnderStructures) Where the map's render-only props begin in props_ (the .scmap's own before them:
  // the simulation's, which it takes away itself); each structure that has cleared some (by its entity id: the frame it
  // was last listed, frameStamp_, and the props it took); and how many structures stand over each prop taken (by its index:
  // it stands again when none does). ClearUnderStructure takes them as a structure is first listed (AddFrame: a unit of the
  // frame, or one remembered under the fog of war); ReleaseClearings, after them, gives back those of the structures not
  // listed this frame (a dying one counts until its death is over). Drawn without them from the next Begin on.
  size_t decorFrom_ = 0;
  struct Clearing {
    uint32_t stamp = 0;
    std::vector<uint32_t> props;
  };
  std::unordered_map<uint32_t, Clearing> clearings_;
  std::unordered_map<uint32_t, int> decorUnder_;
  // [mapfx] A map's own looping effects (its <map>_fx.json beside the .scmap: [{"emitter": blueprint, "at": [x, y, z] (y over
  // the ground), "turn": degrees about the upright, "scale": s}, ...]; the visuals track's plunge-pool foam on SETONS_R),
  // played forever where they stand, started again whenever they are not running (a game begun over clears the particles)
  struct MapEffect {
    std::string emitter;
    Vec3 at;
    Quat turn;
    float scale = 1;
    uint32_t handle = 0;
  };
  std::vector<MapEffect> mapEffects_;
  void KeepMapEffects();
  void ForgetUnderfoot(uint32_t i);  // [simunder]
  void ClearUnderStructure(uint32_t id, const Model* model, Vec3 position, const Quat& orientation);
  void ReleaseClearings();
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
  // [fogglow] the units that died lately, where, when, and whether the army the picture is for ever saw them (a wreck
  // made there takes it: WreckKnown); kept while the fog of war is drawn
  struct KnownDeath {
    Vec3 at;
    double when;
    bool known;
  };
  std::vector<KnownDeath> deaths_;
  std::unordered_set<uint32_t> deathsTold_;
  bool WreckKnown(Vec3 centre, float radius, double born) const;
  bool GlowShown(const PlacedProp& p, Vec3 centre) const;
  Vec3 heatTint_;
  void HoldHot(uint32_t id, PlacedProp* prop);
  void UpdateHotProps();
  // [rockhits] (rockHits on now: the modern look, a map, no picture for something else; the map's rocks found the first time it
  // is, MakeRocks; the hits acted on at this moment of the game's, at and size: a shot's impact and the blast the game makes
  // of it are told apart, at the same place and moment, and act once; the shots seen stopped, when the game's impacts are not
  // kept)
  bool rocksNow_ = false;
  std::vector<Vec4> rockHitsNow_;
  double rockHitsClock_ = -1;
  std::vector<uint32_t> rockShotsStopped_, rockShotsBefore_;
  void MakeRocks();
  static void Sphere(const Model* model, const Mat4& world, Vec3* centre, float* radius);
  bool UnderWater(Vec3 centre, float radius) const;
  // the last hits on each unit's personal shield (at most eight, the newest last): where, on the model at
  // rest (world units), and when
  struct ShieldHit {
    Vec3 at;
    double clock = 0;
  };
  std::unordered_map<uint32_t, std::vector<ShieldHit>> shieldHits_;
  std::unordered_map<uint32_t, double> warpSince_;  // [warpin] (each unit warping in: the clock when its phase mesh was first seen on it)
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
  std::unordered_map<uint64_t, const Model*> shotsByUnit_;  // [byunit] (shell blueprint << 32 | launcher: its own model, or none)
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
    // [fogwar] the rest of the still photo the original keeps of it (RefreshBlip 0x5c1b90: health, max health, fraction
    // complete, mesh, pose), taken at the last frame the focus army saw it: its posed and hidden bones (the newer beat's),
    // its treads' scroll, its health; its scars, dirt and damage as they were (taken at the first frame it was not seen)
    std::vector<FrameBone> bones;
    float treads[2] = {0, 0};
    float health = 0, maxHealth = 0;
    bool wearKept = false, hasWear = false;
    WearLook wear;
  };
  // [fogwar] whether a unit kind is a structure (its blueprint's Physics.MotionType none), by FrameUnit::blueprint:
  // -1 not asked yet, 0 no, 1 yes
  std::vector<int8_t> structureKind_;
  std::unordered_map<uint32_t, Remembered> remembered_;
  // [glitches] (finishedSlabFade) the build glow slabs with no unit under construction under them: since when (clock_), and
  // the frame they were last listed (frameStamp_; gone ones dropped)
  struct LeftSlab {
    double since;
    uint32_t stamp;
  };
  std::unordered_map<uint32_t, LeftSlab> leftSlabs_;
  std::vector<std::pair<Vec3, float>> buildingNow_;  // (this frame's units under construction: place, and how far round, for those)
  uint32_t buildingNowStamp_ = 0;
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
    bool hull = false;     // [hullglow] a unit's own glow (shared with those it lies over: hullShare)
    float air = 1;         // [hullglow] its share in the air (Lights::Add)
    bool building = false; // [buildingglow] a structure's glow, by the structures' rule (shared with theirs alone)
  };
  std::vector<float> hullField_;  // [hullglow] (hullShare) how many hull glows' worth of light lies on each square, FlushLamps
  std::vector<float> hullBuildingField_;  // [buildingglow] the same, of the structures' glows alone
  void ShareHullGlows(const std::vector<uint8_t>& allowed, std::vector<float>* shares);
  std::vector<PendingLamp> pendingLamps_;
  uint32_t pendingUnits_ = 0;
  struct LampLevel {
    float level = 0;
    uint32_t seen = 0;  // (frameStamp_ it was last asked for)
    float share = -1;   // [glowsteady] its hull glow's share where glows overlap, eased (ShareHullGlows; -1: not yet)
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
  // [dents] How large a weapon's shot's hole is (DentGround's size), by its id as the simulation names it ("uel0103-1-MainGun"):
  // from its blueprint's DamageRadius and Damage, read once
  std::unordered_map<std::string, float> dentSizes_;
  float ShotDentSize(const std::string& weapon);
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
  bool stormCells = true;  // [stormcells] a storm's clouds in towering cells (clouds.h, Clouds::storm); off: as before
  bool starryNight = true;  // [starry] the night sky deep, starry, the band and the moon's halo (sky.h, Sky::starry); off: as before
  float stormSunNow_ = 0;  // [stormcells] this frame's storm, for the sun pockets (World::Begin)
  PhotoFinish photoFinish;
  // [horizon] The weather on the horizon (horizon.h; Chris, 2026-10-18, "beyond the map's edge, part 2"): on the sea
  // beyond the map's edge, winter ice while snow lies (floes thickening into pack ice, a few icebergs far out), low
  // fog banks drifting, showers (rain curtains, snow squalls) passing far out, and the clouds' shadows carried over the
  // far sea. Modern look, with the world beyond the edge (worldBeyond) and a time of day; off by default.
  bool horizonWeather = false;
  Horizon horizon;
  BeyondLand distantLand;  // (beyondland) the land far off and the soft edge's look (beyondLand)
  Surround surround;       // (surround) the map's own sculpted land past its edges (mapSurround)
  bool surroundTried_ = false;  // (looked for, for the map open)
  // (surround) its scenery (surroundScenery): each piece's model and place, by squares of kSurroundCell world units (only
  // those with any; each its box and its group of still scenery with the model renderer, made the first time it is in
  // view), read the first time it is wanted (ReadSurroundScenery); from how far past the map's edge to how far its trees
  // thin out to none (for the ground's forest: Surround::treesFrom)
  struct SurroundCell {
    Vec3 lo, hi;
    // (indices into surroundProps_: in three lots, the first drawn out to surroundSceneryTo, the second to half-way,
    // the third to surroundSceneryFrom; each lot's group with the model renderer)
    std::vector<uint32_t> props[3];
    uint32_t group[3] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
  };
  static constexpr float kSurroundCell = 64;
  std::vector<PlacedProp> surroundProps_;
  std::vector<SurroundCell> surroundCells_;
  bool surroundSceneryTried_ = false;
  float surroundTrees_[2] = {0, 0};
  void ReadSurroundScenery();
  void ForgetSurroundScenery();

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

  // [devicelost] (Chris, 2026-11-09, the review's A4) What of a game this world keeps when its graphics device is lost and
  // it is made anew on another: the game itself is the simulation's and is not lost, but the simulation tells a world
  // of some things once only, and those the world has to carry over itself. Keep reads this world's own memory alone
  // (nothing of the device: right also when that is lost); Give lays it on a world of the same map just opened, before
  // its first frame of the game. `tick`: the beat last shown.
  //   Carried over: how the blueprints look (SetLooks); the ground as the game laid it flat; the scenery as it stands
  //   (wreckage where it lies, burnt and felled trees, what was reclaimed gone; a wreck's age, so one still hot goes on
  //   cooling); the scripts' effects that last (Effects::Keep: what hangs on units, the pads under buildings, the
  //   deposits' marks, scorches and craters not yet run out); how wet the ground is and how much snow lies; the
  //   interface's lists and the switches WorldPicture sets (selection, hover, ghosts, shapes, ranges, order lines, meshes,
  //   decals, borders, the playable part, the icons' colours; life bars, work bars, unexplored, icons, selection marks).
  //   Not carried over, because it lives on the card or is the renderer's own doing over the frames, and begun afresh:
  //   the battle scars burnt into the ground and the burnt plants, tracks and dents in the ground and in the snow, the
  //   marks on units (hits, dirt, snow on them), rocks' scars, debris lying, the smoke, fire and dust in the air (what
  //   still burns smokes again), explosions under way, ripples and surf, the lamps' fading, and what the picture keeps
  //   from frame to frame (the temporal upscaler, the light in the air, the clouds, the bounce light: settled again
  //   within a few seconds). The hour and the weather themselves come from the game's clock and its seed: the same.
  //   A host's own switches on the world are the host's to set again, as after WorldPicture::SwapMap.
  struct Kept {
    bool valid = false;
    uint32_t tick = 0;
    std::vector<BlueprintLook> looks;
    std::vector<uint16_t> heights;
    struct Prop {
      FramePropEvent event;   // (what the simulation last said of it; one made in the game: as its making, at its own beat)
      bool glowKnown = true;  // ([fogglow] a wreck's)
    };
    std::vector<Prop> props;                   // in the order of their beats
    std::vector<FramePropEvent> propsPending;  // (told and not yet due)
    std::vector<FrameEffectEvent> effects;
    float wet = 0, snowLying = 0;
    std::unordered_set<uint32_t> selected;
    uint32_t hover = 0xffffffffu;
    int hoverKind = 0;
    std::vector<Ghost> ghosts;
    std::vector<Line> lines;
    std::vector<Ring> rings;
    std::vector<UserDecal> userDecals;
    std::vector<PlacedMesh> placed;
    std::vector<std::string> borders, orderTextures;
    std::vector<OrderLeg> orderLegs;
    std::vector<OrderMark> orderMarks;
    float orderNumbers[4] = {1, 35, 7, 100};  // (lineScale, curveSmoothness, markMinPixels, markMaxPixels)
    int orderSegments = 32;
    std::vector<RangeGroup> rangeGroups;
    float rangeZoom[2] = {10, 1000};
    float playable[4] = {0, 0, 0, 0};
    std::vector<Vec3> iconColors;
    bool lifeBars = false, workBars = true, unexplored = false, iconsOn = true, selectionMarks = true;
  };
  void Keep(Kept* out, uint32_t tick) const;
  void Give(const Kept& kept);

 private:
  // [devicelost] (Keep) the looks given so far (SetLooks hands them on and the simulation gives them once), and what the
  // simulation last said of each prop it changed (by its entity's number; a map's own prop: 2^32 + its place in the map's
  // list), kept as each change takes hold
  std::vector<BlueprintLook> looksKept_;
  std::unordered_map<uint64_t, FramePropEvent> propsKept_;
  bool streamNow_ = false;  // [stream] (streams drawn as water this frame)
  bool horizonNow_ = false;  // [horizon] (something of it this frame)
  Season seasonOpen_ = Season::Summer;  // [seasons]
  bool leavesNow_ = false;              // [seasons] (falling leaves drawn this frame)
  std::vector<Tarmacs::Pad> padsNear_;  // [tarmac] (the remastered pads, for the ground cover cleared under them)
};

}  // namespace render
