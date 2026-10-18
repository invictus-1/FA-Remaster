// Units seen behind trees and cliffs (docs/render-lighting.md, the next looks, item 15): where a unit the player
// may see - his own, an ally's, an enemy's in sight, never one in the fog of war - is hidden behind leaves,
// props, a hill or a building, a faint thin outline in its army's colour (the strategic icons' colours), with a
// fainter fill inside it, shows where it is. Only the hidden part: where the unit itself shows, nothing is drawn.
//
// How it works:
//  - The units the world wants outlined are marked as they are listed (World::MarkOutline, MeshRenderer::MarkLast):
//    a number holding their colour (an index into this frame's palette), which unit it is (a few bits of its id),
//    how strong its outline is (by its size on the screen, gone where the strategic icons take over) and whether
//    it is a building.
//  - After the scene, what the scene drew of them (the same levels of detail) is drawn again (Masks) at the
//    SCREEN's size, by the view without the temporal upscaler's jitter, into pictures of this class's own with a
//    depth of their own: so each pixel holds the NEAREST marked unit there, its mark, and whether that surface
//    lies behind the scene's depth at the place (behind the farthest of the scene's samples there, by a little:
//    the scene may be drawn smaller, or shifted, or with several samples a pixel). A unit's own parts lie in that
//    depth too, so they never count as hiding it.
//  - Two such layers: units that move, and buildings. A unit behind a building is hidden (the building is not in
//    the moving units' layer); a building is outlined where something other than a moving unit hides it (a
//    building behind a tank is not marked: the tank stands in front, seen).
//  - Draw lays the outlines over the finished picture at the screen's size (after the scene is brought up by FSR,
//    before the order lines, the icons and the interface): a pixel of a unit's hidden part is on its line when
//    another unit or nothing lies within the line's width of it, else it gets the fill. Unlit, in the colour as
//    the screen shows it.
// Frame generation (frame_generation.h) takes the outlines as it takes the icons: drawn after the picture it makes
// its frames between from (Post::KeepFinished), so they are never smeared along what hides the unit; they are
// taken from the frame after, as the icons are.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class MeshRenderer;

class HiddenOutlines {
 public:
  explicit HiddenOutlines(gfx::Device* device) : device_(device) {}
  ~HiddenOutlines();
  // Builds its pipelines (gSceneSamples must have its final value: the scene's depth is read by it).
  bool Init(std::string* error);

  // --- a frame ---
  // At the frame's start: the screen's size (the outlines' width is measured by its height); this frame's
  // palette emptied.
  void Begin(int screenWidth, int screenHeight);
  // How strong a unit's outline is (0..1), by its size on the screen: `radius` its bounding sphere's, `distance`
  // from the eye, `fovY` the camera's; gone below `smallest` pixels across (at 1080 lines; the same share of
  // another screen's height), whole from `largest`. `iconShown`: how much of its strategic icon shows (0..1;
  // the outline gives way to it).
  float Strength(float radius, float distance, float fovY, float iconShown) const;
  // The mark for MeshRenderer::MarkLast: `color` as the screen shows it (0..1), whether it is a building, its
  // entity id, its strength (Strength). 0 (no mark) when it is too faint to be seen.
  uint32_t Mark(Vec3 color, bool building, uint32_t id, float strength);
  // After the scene's last solid pass, before its finishing passes (it begins passes of its own): the marked
  // units the scene drew, again, by `screen` (the scene's view at the screen's size, without the jitter), each
  // layer into its picture. `sceneDepth`: the scene's depth target, `sceneWidth` x `sceneHeight` (its samples:
  // gSceneSamples); `zNear` the camera's near distance; `shifted`: the scene is drawn shifted (the temporal upscaler's
  // jitter).
  void Masks(MeshRenderer* meshes, const View& screen, gfx::Texture sceneDepth, int sceneWidth, int sceneHeight, float zNear, bool shifted);
  // The map's ground, before Masks (none: no map): its heights (Terrain::HeightTexture), the map's size and the world
  // units of the heights' largest value. What of a unit lies in the ground, or less than `buried` over it, is not
  // hidden but buried (a building's foot on a slope): it gets no outline.
  void SetGround(gfx::Texture heights, float mapWidth, float mapHeight, float heightUnit) {
    heights_ = heights;
    ground_[0] = heights ? mapWidth : 0;
    ground_[1] = mapHeight;
    ground_[2] = heightUnit;
  }
  // Into the window's pass under way (the finished picture at the screen's size): the outlines, when Masks found
  // something to mark this frame.
  void Draw();

  // --- the look ---
  float width = 1.5f;     // the line's width in pixels at 1080 lines (the same share of another screen's height)
  float line = 0.65f;     // how much of its colour the line lays over the picture (at full strength)
  float fill = 0.1f;      // and the fill inside it
  // Strength's sizes on the screen, in pixels across at 1080 lines (a tank, UEL0201, fades from some 70 world units
  // from the eye; its strategic icon takes over at 130)
  float smallest = 8, largest = 18;
  // How far behind the scene's depth a unit's surface must lie to count as hidden: this share of its distance
  // from the eye, and these world units more.
  float behindShare = 0.008f, behindUnits = 0.05f;
  float buried = 0.12f;  // (SetGround)

  // Of the frame last drawn: units marked, and whether the outlines were laid.
  int marked = 0;
  bool drawn = false;

 private:
  gfx::Device* device_;
  gfx::Pipeline mask_, lay_;
  gfx::Texture masks_[2], depth_;  // (the moving units' layer, the buildings'; their depth, shared in turn)
  int width_ = 0, height_ = 0;     // the targets' size
  int screenWidth_ = 1, screenHeight_ = 1;
  std::vector<Vec3> palette_;      // this frame's colours (a mark holds an index into it, from 1)
  gfx::Texture heights_;           // SetGround's
  float ground_[3] = {0, 0, 1};
  bool layers_[2] = {false, false};  // this frame's marks: some in each layer
};

}  // namespace render
