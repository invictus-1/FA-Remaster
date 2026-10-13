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
  bool Open(const moho::Vfs* vfs, const std::string& mapPath, std::string* error);
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
  // Buildings shown as ghosts while their site is chosen: green where the site is free, red where not.
  struct BuildPreview {
    std::string blueprint;  // the unit's blueprint id ("ueb0101")
    float position[3] = {0, 0, 0};
    float heading = 0;      // radians about the upright axis
    bool free = true;
  };
  void SetBuildPreviews(const std::vector<BuildPreview>& previews);
  // Lines between places of the world and rings lying on the ground (the lines of a unit's orders,
  // ranges). Colours are 0xAARRGGBB; lines are one pixel wide and hidden by what is in front of them.
  struct Line {
    float a[3], b[3];
    uint32_t color;
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
  // The world itself, for its settings (look, shadows, icons, ...). Null before Open.
  World* world();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
