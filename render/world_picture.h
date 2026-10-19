// The world's picture as pixels, for a host that has its own window and its own way of drawing
// (the interface's program with its software backend): the host holds the simulation, tells this
// class about every beat, and asks once a frame for the world as seen by a camera, as a block of
// pixels it then shows under its own controls. The same object answers where things are on that
// picture and what is under a point of it.
//
// This is the first, simple way to join the two (docs/render-answers.md). It costs a read-back of
// the picture from the graphics card every frame; the way without that is one window and one
// device for both, the interface's controls drawn by the renderer's device on top of the world.
//
// Use, with a simulation `sim` and the game's files mounted in `vfs`:
//   sim.collectEffects = true;                       before the simulation's first beat
//   sim.collectProps = true;                         the same, for the scenery's changes (wreckage, trees)
//   sim.collectHits = sim.collectImpacts = sim.collectHealth = true;
//                                                    the same, for the looks that answer hits (marks on units,
//                                                    ripples, blast waves; about 1% of a beat's time)
//   render::WorldPicture picture;
//   picture.Open(&vfs, "/maps/SCMP_009/SCMP_009.scmap", &error);
//   picture.GameStarted(sim);                        once the session has started
//   picture.Beat(sim);                               after every Tick() (and once for the state at the start)
//   const std::vector<uint32_t>& pixels = picture.Draw(camera, width, height, tick);
// `tick` is the moment to show, in ticks; give the simulation's tick plus the part of the next
// beat that has passed (0..1) and units move smoothly between beats. The picture always shows a
// moment that lies one beat back, between the last two beats it was told of, because the place of
// a unit between two beats can only be known once the second one exists.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "camera.h"
#include "cinecam.h"
#include "linalg.h"
#include "season.h"  // [seasons]

namespace moho {
class Sim;
class Vfs;
}  // namespace moho

namespace render {

class World;

class WorldPicture {
 public:
  WorldPicture();
  ~WorldPicture();

  // Makes an invisible window and a graphics device and loads the map. `vfs` must outlive this.
  // After UseWindow: the device draws into the host's window instead (no invisible window).
  bool Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error);

  // --- the picture straight into the host's window (the way without a read-back) ---
  // With UseWindow and an empty map path, Open makes no world: DrawToWindow then shows the interface
  // alone (the menu's screens; its moving background drawn by another WorldPicture into the layer).
  // A window takes one such picture at a time: close one (destroy the WorldPicture) before the next
  // opens on it - the menu's before the game's, the game's before the menu's again. (A window that
  // has shown pictures this way does not show the host's GDI drawing any more.)
  // Before Open: the device is made on the host's window `hwnd` (its whole client area) and shows
  // the picture there itself, through the graphics card's swap chain (DrawToWindow); nothing is
  // read back and the window is not to be drawn into by the host otherwise. Draw is then not to be
  // used; DrawMinimap still is (its ground is drawn without being shown).
  void UseWindow(void* hwnd);
  // The same for a host on another system (Vulkan; written to the specification, untried: there is no
  // such system here): its window as its window system has it, `system` as gfx::DeviceDesc::WindowSystem
  // (1 Xlib: `window` the X11 Window id, `display` the Display*; 2 Wayland: the wl_surface* and the
  // wl_display*; 3 Metal: a macOS view's CAMetalLayer*, no display), at `width` x `height` pixels.
  void UseNativeWindow(int system, void* window, void* display, int width, int height);
  // The same where the host makes the window's Vulkan surface itself (SDL3 on Linux and macOS:
  // SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface), before Open, in place of UseWindow and
  // UseNativeWindow: the instance extensions its window needs, and a function that makes the window's surface
  // for the VkInstance it is given (as a void*) and returns the VkSurfaceKHR as a 64-bit value (0: it could not).
  // The picture draws with Vulkan then (SetVulkan is not needed). `width` x `height`: the drawable's size in
  // pixels at first; later sizes come with each DrawToWindow, as with the other windows.
  void UseHostSurface(std::vector<std::string> instanceExtensions, std::function<uint64_t(void* vkInstance)> createSurface, int width, int height);
  // Before Open: draws with Vulkan in place of Direct3D 12 (being tried: docs/render-vulkan.md).
  void SetVulkan(bool on);
  // (checks) Before Open: the graphics API's validation layer, where installed (the viewer's --debug).
  void SetDebug(bool on);
  // The host's interface, laid over the world by the next DrawToWindow: `width` x `height` pixels
  // (the window's size), each 0xAARRGGBB with the colour already multiplied by its alpha (alpha 0:
  // the world shows through), `pitch` pixels from one row to the next. (x, y, w, h) is the part that
  // has changed since the last call: all of it the first time and after a change of size; w or h 0:
  // nothing has. The pixels must stay where they are until that DrawToWindow.
  void SetInterfaceLayer(const uint32_t* pixels, int width, int height, int pitch, int x, int y, int w, int h);
  // The world as Draw draws it, the interface over it, shown in the window.
  void DrawToWindow(const Camera& camera, int width, int height, double tick);
  // DrawToWindow waits for the screen's refresh (vertical sync, as at first) or not.
  void SetVsync(bool on);
  // "Low Latency" (the Video tab's; off as at first, a player's option): the camera, the cursor's feedback and the
  // interface answer the mouse and the keys sooner (gfx.h, Device::SetLatencyLimiter). The frames waiting to be shown
  // are kept to one and each frame begins just in time for its refresh; with it, the host calls WaitForNextFrame once
  // a frame, just before it reads its input (its window's events, the mouse's place), and then draws the frame
  // (DrawToWindow): the wait is there, so that what it reads is as fresh as the frame allows. Set before or after
  // Open; only for the picture shown in the host's window.
  void SetLowLatency(bool on);
  // Once a frame, just before the host reads its input. With Low Latency off it returns at once.
  void WaitForNextFrame();
  // For checking: the next DrawToWindow also reads what it shows back into `out` (RGBA bytes).
  void CaptureNextWindowFrame(std::vector<uint8_t>* out);
  void GameStarted(const moho::Sim& sim);
  void Beat(const moho::Sim& sim);

  // The world as `camera` sees it, `width` x `height` pixels, each 0xAARRGGBB, row by row from the
  // top (the form of the interface's moho::ui::Image). Valid until the next Draw.
  const std::vector<uint32_t>& Draw(const Camera& camera, int width, int height, double tick);

  // --- what the interface shows in the world; each stays as set until it is set again ---
  // The selected units, by entity id: a ring on the ground under each, its strategic icon in the
  // selected form.
  // The army the picture is for (the engine's number, from 1; 0: no one army, everything shows, as
  // at first). From the next beat on, other armies' units show as that army knows them (the
  // engine's ReconOf): the unit while it is seen, its blip (the strategic icon without detail, at
  // any distance) while it is on radar, sonar or omni alone, nothing otherwise; their shots only
  // where the army sees. The minimap, UnitAt and UnitsIn go by the same.
  void SetFocusArmy(int army);
  // The game's option FogOfWar: true for "unexplored" (ground the focus army has never seen is all
  // but black, in the picture and on the minimap, until it is seen); false, as at first, for
  // "explored" (it looks like ground seen before). What the army has seen is kept from when it
  // became the focus army.
  void SetUnexplored(bool on);
  void SetSelection(const std::vector<uint32_t>& ids);
  // The unit under the mouse: marked with the original's brackets of that kind (1 the player's own
  // or an ally's, 2 an enemy's, 3 anyone else's). Kind 0 or an id that is no unit's: no mark. A
  // selected unit keeps its selection brackets.
  void SetHover(uint32_t entity, int kind);
  // Life bars over units (selected, under the mouse, or damaged and near): OFF at first; a player's
  // switch for them is the interface's.
  void SetLifeBars(bool on);
  // Life bars go over the units of the army the picture is for and of the armies allied with it
  // (the engine's numbers, from 1; none at first), as the original's; over the enemy units the
  // army sees now only with SetLifeBarsOnEnemies (the console's ui_ForceLifbarsOnEnemy, FAF's
  // "Always Show Enemy Lifebars": OFF at first, as the original). For no one army: every unit's.
  void SetAllies(const std::vector<int>& armies);
  void SetLifeBarsOnEnemies(bool on);
  // The game's effects remastered (the visuals track's files where it has made some: true, as at
  // first) or as the original has them (false). A player's option.
  void SetRemasteredEffects(bool on);
  // The effects quality setting: the share of their particles effects throw out (1, as at first, all;
  // 0.5 for a slower card). A player's option.
  void SetEffectsQuality(float share);
  // AMD's FSR 1 (upscale.h): the scene drawn at `share` of the picture's width and height and brought up
  // to it: 1 off (as at first), 1/1.3 "Ultra Quality", 1/1.5 "Quality", 1/1.7 "Balanced", 1/2
  // "Performance". Icons, bars and the interface stay sharp. A player's option, for slower cards.
  void SetUpscale(float share);
  // AMD's FSR 3 (fsr3.h) in place of FSR 1, at SetUpscale's share: the scene drawn a little shifted every
  // frame and brought up from what the frames before saw; at a share of 1 it smooths the edges at the
  // picture's own size, in place of anti-aliasing. Needs anti-aliasing off (gSceneSamples 1 before Open);
  // with it on, FSR 1 stays. Off as at first. A player's option.
  void SetTemporalUpscale(bool on);
  // Reflections by ray tracing (World::rayReflections): smooth surfaces and the water mirror what their rays meet,
  // what is off the screen too. The surfaces' shaders are built for it when a map is opened (gRayShaders), so it
  // takes effect from the next Open; switched off later it stops at once. Only where the device has ray tracing
  // (Direct3D 12 with DXR 1.1, Vulkan with ray queries; never on the Mac); elsewhere nothing changes. Off as at
  // first. A player's option. The other lighting options are the world's own switches (world(); their costs in
  // docs/render-lighting.md, "The options").
  void SetRayReflections(bool on);
  // The lighting upgrade's options in the four bundles the game's settings show (Chris, 2026-10-17: all of them
  // in, bundled where they go together); each off as at first, a player's option, the modern look's; set before
  // or after Open, each stays as set. Costs at 1440p on an RTX 4090 (docs/render-lighting.md, "The options"):
  // Atmosphere: light in the air, volumetric clouds, the sky from tables (World::volumetricFog, volumetricClouds,
  //   skyTables); with a time of day (SetDayAndWeather); together about 0.25 ms cheaper than without. Since
  //   2026-10-18 also the zoom out through the volumetric clouds (World::cloudZoom, a trial; Chris: every new look
  //   on): +0.4 to 1.0 ms only with the camera at the clouds' layer or above. And the land far off past the map's
  //   edge with a softer edge (World::beyondLand; where the world beyond the edge is shown): 0.03-0.19 ms.
  void SetAtmosphere(bool on);
  // Soft shadows: the sun's shadows soft by their distance from what casts them, and contact shadows
  //   (World::softShadows, contactShadows); about +0.37 ms.
  void SetSoftShadows(bool on);
  // Ray tracing: bounce light, reflections and the small lights' shadows (World::bounceLight, SetRayReflections,
  //   rayLightShadows: reflections and the shadows from the next map opened, their shaders being built then); about
  //   +0.75 ms, and the shadows' cost by how many small lights are in view (docs/render-lighting.md); only where
  //   RayTracingSupported, elsewhere nothing changes.
  void SetRayTracing(bool on);
  // Weather effects: rain and snow lit where they fall, rain's splashes, wet shorelines with stronger caustics
  //   (World::litRain, rainSplashes, waterDetail), and each unit's own snow, shed in combat (World::unitSnow);
  //   about +0.05 ms while it rains. And the weather on the horizon (World::horizonWeather, proposed here: winter
  //   ice, fog banks and showers on the sea beyond the map's edge).
  //   And units wet in rain and muddy from soft ground, drying and flaking off (World::unitDirt): about 0.01-0.02 ms.
  //   And surf and sea spray (World::surf): waves breaking on the shores, spray on rocks and off ships' bows; the water
  //   up to +0.1 ms with surf over a third of the screen.
  void SetWeatherEffects(bool on);
  // Ground detail: grass near the camera, pressed flat round units and buildings (World::groundCover); about
  //   +0.05 to 0.2 ms when zoomed in, nothing from afar; and battle scars (World::battleScars): the scorches and
  //   craters of the fighting stay on the ground, weathering, up to 2000; light through leaves (World::leafLight):
  //   the sun shines through leaves, grass and plants; deep tracks (World::deepTracks): the ground pressed
  //   where units drive and walk, by the kind of ground it is; wildlife (World::wildlife): birds over the
  //   woods and shores that scatter from fighting, fireflies at dusk; and plants that react (World::plantsReact,
  //   [plants]): grass and bushes pressed over where units drive and springing back, burnt where blasts and fires
  //   were and growing back, felled trees still, burnt ones bare.
  void SetGroundDetail(bool on);
  // FSR 3 frame generation (World::frameGeneration): a picture made between each two frames and shown between
  // them, evenly apart, so twice the frames are shown, for a little more than half a frame's delay; the icons and
  // the interface stay sharp (taken from the frame after). Best where the game already runs at 60 or more. Needs
  // anti-aliasing off (gSceneSamples 1 before Open), as SetTemporalUpscale; with it on, or where the device
  // cannot, nothing changes. Set before or after Open (from the next frame). Off as at first. A player's option.
  void SetFrameGeneration(bool on);
  // Units seen behind trees, cliffs and buildings (World::hiddenOutlines, hidden_outlines.h): where a unit the player
  // may see - his own, an ally's, an enemy's in sight, never one in the fog of war - is hidden behind leaves, props, a
  // hill or a building, a faint thin outline in its strategic icon's colour shows where it is; it gives way to the
  // strategic icons from afar. ON at first (Chris, 2026-10-17: readability); the modern look's (the original look has
  // none). A player's option, set before or after Open. Cost at 1440p: docs/render-lighting.md, item 15.
  void SetHiddenOutlines(bool on);
  // After Open (of any picture, the menu's too): whether this device has ray tracing (Direct3D 12 with DXR 1.1,
  // Vulkan with ray queries; never on the Mac), for greying SetRayTracing out where it would do nothing.
  bool RayTracingSupported() const;
  // The texture detail setting (TextureCache::detail): 0 all sizes (as at first), 1 the largest left out
  // (half width and height, a quarter of the memory), 2 the two largest. Before Open: it applies to what
  // the next Open loads. The sun's shadows' detail is the world's `shadowSize` (world()->shadowSize).
  void SetTextureDetail(int drop);
  // The thin bar of a unit's work (a build, an upgrade, a commander's enhancement: how far it is)
  // under where its life bar is, and the game's pause mark over a unit the player has paused. For
  // the units of the army the picture is for (SetFocusArmy; every army's when it is for none).
  // On at first, and not switched by SetLifeBars.
  void SetWorkBars(bool on);
  // A day that passes and weather that changes, both by the game's own clock (the `tick` given to
  // Draw), so that every player of a game sees the same and a paused game's day stands still.
  // `startHour`: the hour the game starts at, 0..24 (6 sunrise, 12 noon, 18 sunset); negative, as
  // at first: the map's own fixed light, no weather. `dayLengthSeconds`: how long a whole day
  // takes in game time; 0: the hour stands still. `weather`: it changes by itself in spells of a
  // few minutes (clear, cloudy, rain, storm, fog, snow), drawn from the game's seed; false: fair.
  // It is only seen; it does nothing to the game.
  void SetDayAndWeather(float startHour, float dayLengthSeconds, bool weather);
  // [seasons] The season the map is drawn in (season.h; seasons.h says what each changes): Summer as at first;
  // Autumn: where the map's remastered ground lists an autumn look (SETONS_R), its ground's materials, its ground
  // cover and its props in their autumn art, leaves drifting off the autumn trees in the wind, and with
  // SetDayAndWeather's changing weather more overcast, showers and morning fog. Taken in by Open: a change shows from
  // the next map opened. The picture's alone (the simulation keeps the map's blueprints): every player may pick his own.
  // A player's option.
  void SetSeason(Season season);
  // Buildings shown as ghosts while their site is chosen: green where the site is free, red where not.
  struct BuildPreview {
    std::string blueprint;  // the unit's blueprint id ("ueb0101")
    float position[3] = {0, 0, 0};
    float heading = 0;      // radians about the upright axis
    bool free = true;
  };
  void SetBuildPreviews(const std::vector<BuildPreview>& previews);
  // Pictures laid on the ground (the game's UserDecal: the ring of a weapon's splash while the player
  // aims, a nuke's inner and outer rings): a texture of the game's files ("/textures/ui/common/game/
  // AreaTargetDecal/weapon_icon_small.dds"), its middle on the map and its size across in world units.
  // Square to the map, north up, in their own colours, under the units. The list stands until the next call.
  struct UserDecal {
    std::string texture;
    float x = 0, z = 0;
    float sizeX = 1, sizeZ = 1;
  };
  void SetUserDecals(const std::vector<UserDecal>& decals);
  // The strategic icons' colours by army (from 0, as the armies' own colours are), 0xAARRGGBB, in place of
  // the armies' own: the game's "team colour" button (TeamColorMode). Only the icons change (the minimap's
  // too); the models keep their army's colour; a blip not yet known stays grey. Empty: the armies' own.
  void SetIconColors(const std::vector<uint32_t>& argbByArmy);
  // The part of the map that can be played now (the campaign's: ScenarioFramework.SetPlayableArea, the
  // user side's SyncPlayableRect), in world units, x0 < x1 and z0 < z1; the whole map at first, and
  // again when x1 <= x0. Outside it nothing of the game is shown: no unit, no strategic icon, no bar,
  // and UnitAt / UnitsIn do not find them there. The picture there is black, as the original's (its
  // boundary pass blackens everything that lies there: ground, water, units); in the modern look with
  // the world beyond the map's edge (World::worldBeyond, the "no machinery" choice) it is dimmed instead,
  // behind a thin dark line along the edge. The minimap shows this part alone (see DrawMinimap).
  void SetPlayableRect(float x0, float z0, float x1, float z1);
  // The cinematics' and the score screen's switches (gamemain.lua, NIS mode), each on at first:
  // the strategic icons (UI_NisRenderIcons; the minimap keeps its own), the selection's and the
  // mouse's brackets (ren_SelectBoxes), and the world itself (ren_Oblivion: off = Draw and
  // DrawToWindow show black under the interface; the game goes on underneath, its effects too).
  void SetStrategicIcons(bool on);
  void SetSelectionMarks(bool on);
  void SetWorldShown(bool on);
  // Lines between places of the world and rings lying on the ground (the lines of a unit's orders,
  // ranges). Colours are 0xAARRGGBB; lines are hidden by what is in front of them. A line's width
  // is in pixels on the screen, the same from near and from far (1: a hairline, as before).
  struct Line {
    float a[3], b[3];
    uint32_t color;
    float width = 1;
  };
  struct Ring {
    float centre[3];
    float radius;
    uint32_t color;
  };
  void SetShapes(const std::vector<Line>& lines, const std::vector<Ring>& rings);
  // The order lines while Shift is held, as the original draws them (order_lines.h; the interface works out the
  // legs between the orders' places, their units, states and colours, from the game's commandgraphparams.lua):
  // each leg a flat textured ribbon on a curve, as wide as its units say and a few pixels more, seen through
  // hills and units, the first leg given keeping a pixel where legs cross; each mark a waypoint's icon on the
  // ground. `textures`: their paths, the legs' and marks' `texture` an index into it (-1: none). Stays as set until
  // set again (empty when Shift is let go). The original's console variables: OrderLineSettings.
  struct OrderLeg {
    float a[3], b[3];      // its two places, on the ground
    float ta[3], tb[3];    // the curve's directions there, unit length (zero: straight)
    float baseWidth = 1;   // world units: min(sqrt(units on it), 10)
    uint32_t color = 0xffffffff;  // 0xAARRGGBB
    int texture = -1;
    float uvAspect = 1, animRate = 0;  // the texture along it: U per width of length; U moved each game tick
  };
  struct OrderMark {
    float position[3];
    int texture = -1;      // -1: a solid square
    uint32_t color = 0xffffffff;
    float size = 1;        // world units: the node's size times its waypoint scale
  };
  void SetOrderLines(const std::vector<std::string>& textures, const std::vector<OrderLeg>& legs, const std::vector<OrderMark>& marks);
  // ui_WaypointLineScale, ui_CurveSmoothness, ui_CurveSegments, ui_MinWaypointSize, ui_MaxWaypointSize (the game's
  // Lua: 1, 35, 32, 7, 100, as at first).
  void SetOrderLineSettings(float lineScale, float curveSmoothness, int curveSegments, float markMinPixels, float markMaxPixels);
  // Range rings as the original draws them: all the rings of a group show as ONE outline round
  // the ground they cover together (a ring inside what the others cover does not show); an inner
  // radius cuts a hole with an outline of its own. On the ground, under what stands on it, and on
  // the water's surface. A group is one of the game's range filters in one of its colours.
  struct RangeRing {
    float centre[3];
    float radius;
    float innerRadius;  // 0: none
  };
  struct RangeGroup {
    uint32_t color = 0xffffffffu;  // 0xAARRGGBB; the first byte is not used (give the glow below)
    int glow = 0;                  // 0 (none) .. 255: brighter than the screen can show, so it glows
    // The line's thickness in world units at the nearest and at the farthest zoom (the script's
    // Outer and Inner pairs): of the outline round the group, and of the holes' outlines.
    float outerNear = 0.04f, outerFar = 4.0f;
    float innerNear = 0.04f, innerFar = 4.0f;
    std::vector<RangeRing> rings;
  };
  // `zoomNear`, `zoomFar`: the camera's distances that count as the nearest and the farthest zoom
  // (the thickness goes evenly between them and stays at its end values beyond).
  void SetRanges(const std::vector<RangeGroup>& groups, float zoomNear = 10, float zoomFar = 1000);
  // The meshes the game's script places in the world (its WorldMesh, and the mark where an order
  // lands): the whole list each time it changes; it is drawn until the next call. By the shader's
  // name: "CommandFeedback", "CommandFeedback2", "RallyPoint" (unlit, over everything, larger
  // from afar, shrinking or growing over their lifetime, the feedback ones fading) and "FakeRings"
  // (unlit, hidden by what is in front, showing as much as its fraction-complete number says) are
  // drawn as the original's techniques of those names; anything else as an ordinary lit model in
  // `color`.
  struct WorldMesh {
    std::string mesh, texture, shader;  // MeshName, TextureName, ShaderName
    float uniformScale = 1;
    float position[3] = {0, 0, 0};
    float orientation[4] = {0, 0, 0, 1};  // a quaternion x, y, z, w
    float scale[3] = {1, 1, 1};
    uint32_t color = 0xffffffffu;  // 0xAARRGGBB
    bool hidden = false;
    float lifetime = 0;            // SetLifetimeParameter's seconds (0: not set) ...
    float age = 0;                 // ... and how many of them have passed since it was set
    float fractionComplete = 1;    // SetFractionCompleteParameter
  };
  void SetWorldMeshes(const std::vector<WorldMesh>& meshes);
  // What the map lies in (the game's MapBorderAdd): the mesh blueprints to draw around the map, as
  // the original places them; empty: nothing. In the main picture only.
  void SetBorders(const std::vector<std::string>& meshBlueprints);

  // --- the minimap: the whole map from straight above with every unit's strategic icon ---
  // `width` x `height` pixels as Draw gives them. The map's whole height fills the picture's height;
  // give a width in the map's proportion (MapSize) and the map fills it exactly. With a playable
  // part set (SetPlayableRect) the same for that part: its height fills the picture's, give a width
  // in its proportion. The ground is drawn
  // by the graphics card once per size and kept; the icons are laid on it on the processor at
  // every call, from the units of the last Draw (selected ones in their selected form).
  const std::vector<uint32_t>& DrawMinimap(int width, int height);
  // Where a place of the world is on the minimap last drawn, and the place on the ground under a
  // point of it.
  bool MinimapProject(Vec3 world, float* x, float* y) const;
  bool MinimapGroundAt(float x, float y, Vec3* hit) const;
  // The map's size in world units.
  void MapSize(float* width, float* height) const;

  // For the picture last drawn; x and y in its pixels.
  // Where a place in the world is on the picture. False when it is behind the camera.
  bool Project(Vec3 world, float* x, float* y) const;
  // The place on the ground (or the water's surface) under a point of the picture.
  bool GroundAt(float x, float y, Vec3* hit) const;
  // Where a unit will be drawn by the Draw, DrawToWindow or LifeBarAnchors given the same `tick`: its
  // place (world units) and its turn as a quaternion x, y, z, w (y up; turned by it, the unit's +z is
  // where it faces), blended between the beats as the picture blends them, not the beat's own. For a
  // camera that follows a unit (a cinematic's TrackEntities, NoseCam): ask first, then draw with the
  // camera made from it. False when there is no such unit at that moment (gone, or not yet there).
  // It does not say whether the army the picture is for sees the unit.
  bool UnitTransform(uint32_t id, double tick, float position[3], float orientation[4]) const;
  // The entity id of the unit under a point of the picture; 0xffffffff: none.
  uint32_t UnitAt(float x, float y) const;
  // The units whose middle lies in a rectangle of the picture.
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const;

  // The height of what one stands on at a place of the map (ground or water surface).
  float GroundHeight(float x, float z) const;

  // --- the cinematic camera (cinecam.h), for watching replays and taking pictures ---
  // Draw and DrawToWindow take its camera as any other (CineCamera::View, Camera::cinematic set): the world is then
  // drawn as seen from within it (the sun's shadows along the view out to the distance, the grass ahead of the eye).
  // The strategic symbols and the deposits' icons are left out for it (far off toward the horizon every unit would
  // stand there as one); the rest of the interface in the world is the host's to say as for the game's camera, and a
  // cinematic view wants none of it: SetSelectionMarks(false), SetLifeBars(false), SetWorkBars(false), no shapes.
  // The height one stands on anywhere, in the map and beyond its edge (where the ground goes on mirrored, World::
  // worldBeyond): for CineCamera::Update and View. Valid while this picture is.
  GroundFn CineGround() const;
  // The lens's depth of field (Lens: off at first; on, the aperture, the focus or auto-focus, how fast it follows),
  // from the next frame; it stays as set. A cinematic camera's, but it works with any.
  void SetLens(const Lens& lens);
  // A cut (CineCamera::TakeCut, after a Jump or a path begun without easing in): the next frame keeps nothing of
  // the frames before (the temporal upscaler's, frame generation's), and the lens focuses at once.
  void CameraCut();
  // A photo: the world as `camera` sees it at `tick` (as Draw), `width` x `height` pixels, which may be larger than
  // the window (up to 16384 a side, and as the card's memory allows: larger asks are made smaller in proportion and
  // `*madeWidth` x `*madeHeight` say what came), without anything of the interface (no icons, bars, marks, order
  // lines, shapes, ghosts, rings or the script's meshes), drawn `supersample` times larger again and made smaller
  // for smooth edges (0: as the memory allows, at most 2), with the lens as set. `settleFrames` frames are drawn
  // first at that size and moment, for what settles over frames (the volumetric clouds, the light in the air).
  // Pixels as Draw gives them (0xAARRGGBB, rows from the top). Between frames (not between LifeBarAnchors and the
  // Draw after it). The next Draw or DrawToWindow goes back to the window's size; it costs some frames' time.
  bool TakePhoto(const Camera& camera, int width, int height, double tick, std::vector<uint32_t>* pixels, int* madeWidth, int* madeHeight,
                 int supersample = 0, int settleFrames = 8);
  // The size a photo asked for comes out at, and how many times larger again it is drawn (TakePhoto's rule; the
  // scene's samples a pixel, gSceneSamples, count: they take memory). For a host that draws photos itself.
  static void PhotoSize(int* width, int* height, int* supersample);

  // --- (photomode) the photo mode (the screen is the interface's: docs/render-answers.md "To ui: a photo mode") ---
  // The free camera is CineCamera::Fly (cinecam.h); the lens is SetLens.
  // A moment of the picture's own: the picture alone shows `hour` (0..24) and the kind of weather `weather` (a number of
  // WeatherKinds) at `strength` (0..1: how hard it rains or snows and how thick its fog is; the clouds are the kind's own;
  // the ground wet or snowed on to match), eased over about a second from what is shown, so that the volumetric clouds
  // and the light settle; the game's own clock and weather go on underneath. With the map's own fixed light (no time of
  // day) the moment is shown anyway: it switches the day on for the picture. The modern look's (the original look has
  // no hours and no weather). Stays until set again or cleared; a photo taken meanwhile shows it settled.
  void SetPhotoMoment(float hour, int weather, float strength);
  // The game's own hour and weather in the picture again, at once (the light in the air and the volumetric clouds then
  // settle over a few frames, as they do whenever the weather changes).
  void ClearPhotoMoment();
  // The moment set; with none, the game's own as last drawn: its hour (-1: the map's own fixed light), the kind of
  // weather nearest to what it is now and that kind's strength.
  void GetPhotoMoment(float* hour, int* weather, float* strength) const;
  using WeatherNames = std::vector<std::string>;
  // The kinds of weather by their number: clear, fair, cloudy, overcast, rain, storm, fog, snow.
  static WeatherNames WeatherKinds();
  // What is under a pixel of the picture last drawn (x, y in its pixels): its distance from the eye along the line of
  // sight, as the lens measures its focus (Lens::focus: picture.SetLens with it focuses there), the water's surface where
  // the line of sight meets it first; 0 for the sky (Lens::focus 0 is auto-focus: for the far distance give a large one),
  // and while a photo is being made. The scene's depth there read back: a small frame of its own between frames (0.2 to
  // 3 ms measured; the first call some 60 ms more, its shader made).
  float FocusAt(float x, float y);
  // A photo made over the next frames: as StartPhoto's camera sees it at `tick`, `width` x `height` pixels (at most
  // 16384 a side, made smaller in proportion), drawn `supersample` times larger again and made smaller for smooth edges
  // (0: twice where that fits the card's memory, else not; 1..4 as asked), with the lens and the moment as set, nothing
  // of the interface in it. Each Draw or DrawToWindow after it draws one frame of the photo (its settling frames first,
  // as TakePhoto's) and then shows the world as it was when the photo began (the interface's layer over it as given), so
  // the window goes on being drawn, its camera held; then the photo is put together on a thread of its own, while the
  // window is drawn as ever. Every frame of a photo shows the game's moment its first showed, also while the game goes on
  // (multiplayer). Where one picture that large does not fit the card's memory (TakePhoto's rule), it is drawn in tiles
  // (photo.h). False when a photo is being made already, or there is no world.
  bool StartPhoto(const Camera& camera, int width, int height, double tick, int supersample = 0);
  // How far the photo has come, 0..1 (1: done, until taken); 0 when none is being made (back to 0 with TakeDonePhoto
  // false: it could not be made, the card had no room for its picture).
  float PhotoProgress() const;
  // Once the photo is done: its pixels (0xAARRGGBB, rows from the top) and size, handed over (true once). Ask once a frame.
  bool TakeDonePhoto(std::vector<uint32_t>* pixels, int* width, int* height);
  // What the last photo took, for a host's numbers: the seconds its frames took (from StartPhoto to the last read back)
  // and putting it together took, the card's memory at most meanwhile (as the device counts it), how it was drawn (tiles
  // across and down, each one's size, drawn larger by) and its frames.
  struct PhotoNumbers {
    double drawSeconds = 0, assembleSeconds = 0;
    uint64_t mostCardBytes = 0;
    int columns = 1, rows = 1, tileWidth = 0, tileHeight = 0, by = 1, frames = 0;
  };
  PhotoNumbers LastPhotoNumbers() const;
  // (checks) The card's memory one picture of a photo may take, bytes, in place of TakePhoto's rule (2.5 GB); 0: the
  // rule again. A smaller one makes photos be drawn in tiles that would fit in one picture (the viewer's --photo-test).
  static void SetPhotoBudget(double bytes);
  // The ground's own height at a place of the map, under water the bottom's: the simulation's
  // TerrainHeight (its own function, on the same heights), as the picture has them, the map's and
  // where the game has laid them flat (taken in with the frames drawn).
  float TerrainHeight(float x, float z) const;
  // The world itself, for its settings (look, shadows, icons, ...). Null before Open.
  World* world();

  // The life bars the next picture will have: for every unit with one, the middle of its bars (life,
  // shield, work) and the lower edge of the lowest, in the picture's pixels: where the original puts
  // a unit's custom name (centred on x, a line's height below y). It prepares the frame of that
  // camera, size and moment, and the next DrawToWindow (or Draw) draws that prepared frame, so the
  // anchors and the bars are of the same moment. Call it after the frame's other settings (selection,
  // hover, ...), with what DrawToWindow will be given.
  struct LifeBarAnchor {
    uint32_t unit;  // entity id
    float x, y;
  };

  // What the last picture drawn cost and held, for a host's overlay of numbers (the interface's).
  struct Stats {
    // the processor, milliseconds: waiting for the card to have a frame free, making the frame
    // ready (the beats blended, units, effects listed), drawing it (handing it to the card), and
    // ending it (the interface laid over it, shown)
    float waitMs = 0, prepareMs = 0, drawMs = 0, endMs = 0;
    // the card, the newest frame it has finished: the whole of it and its stages in their order, ms
    float gpuMs = 0;
    std::vector<std::pair<const char*, float>> gpuStages;
    int drawCalls = 0;
    int models = 0;  // drawn in the scene (units, scenery, effects' meshes)
    long long triangles = 0, shadowTriangles = 0, groundTriangles = 0;
    int particles = 0, emitters = 0;
    long long textureBytes = 0;  // the textures made (all their sizes)
    long long cardBytes = 0;     // the card's memory this program uses (0: not known)
    int width = 0, height = 0;
  };
  const Stats& LastStats() const;
  void LifeBarAnchors(const Camera& camera, int width, int height, double tick, std::vector<LifeBarAnchor>* out);

 private:
  void RenderWorld(const Camera& camera, int width, int height, double tick);
  void PrepareWorld(const Camera& camera, int width, int height, double tick);  // (RenderWorld up to the drawing)
  void TakeStats(float endMs);  // (after a frame has ended)
  void PhotoStep();             // (photomode: one frame of the photo being made)
  void ShowHeld(int width, int height);  // (photomode: the window as it was when the photo began, the interface over it)
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
