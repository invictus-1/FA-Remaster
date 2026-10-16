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
#include <memory>
#include <string>
#include <vector>

#include "camera.h"
#include "linalg.h"

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
  // Before Open: draws with Vulkan in place of Direct3D 12 (being tried: docs/render-vulkan.md).
  void SetVulkan(bool on);
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
  // give a width in the map's proportion (MapSize) and the map fills it exactly. The ground is drawn
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
  // The entity id of the unit under a point of the picture; 0xffffffff: none.
  uint32_t UnitAt(float x, float y) const;
  // The units whose middle lies in a rectangle of the picture.
  void UnitsIn(float x0, float y0, float x1, float y1, std::vector<uint32_t>* out) const;

  // The height of what one stands on at a place of the map (ground or water surface).
  float GroundHeight(float x, float z) const;
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
