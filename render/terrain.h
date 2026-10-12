// The ground: the map's heightfield drawn with level of detail, textured and lit the way the original
// game does it (its "TTerrain" and "TTerrainXP" looks).
//
// Geometry: the heights live in a texture; the mesh is one small grid of quads drawn many times, each
// time for a square patch of the map at some level of detail (level k: quads of 2^k cells). Which
// patches are drawn at which level is decided per frame from the camera (the scheme known as CDLOD):
// near ground gets small quads, far ground large ones, and within each level the vertices slide
// toward the next coarser grid as distance grows, so levels meet without cracks or popping.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "scmap.h"
#include "textures.h"
#include "view.h"

namespace render {

class Terrain {
 public:
  Terrain(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}

  // Builds everything for a map. `map` must stay alive while the terrain is used.
  bool Load(const MapData* map, std::string* error);
  // Into the scene, or (`shadowPass`) into a shadow map, where only depth is written.
  void Draw(const View& view, bool shadowPass = false);

  // On-screen size, in pixels, that one quad of the mesh should have: smaller means more triangles.
  float quadPixels = 6;
  // The map's water depth picture and the colour ramp the ground under water is tinted with
  // (decals lying under water take the same tint).
  gfx::Texture WaterMap() const { return waterMap_; }
  gfx::Texture WaterRamp() const { return waterRamp_; }
  // Numbers from the last Draw.
  int drawnPatches = 0;
  int drawnTriangles = 0;

 private:
  struct Level {
    int nodeCells = 0;         // side of a node of this level, in cells
    int nx = 0, nz = 0;        // nodes along x and z
    std::vector<float> lo, hi; // lowest and highest ground in each node
  };
  struct Patch {
    float x, z, quad, level;
  };

  bool Select(const View& view, int level, int ix, int iz);
  void AddNode(const View& view, int level, int ix, int iz);
  void AddPatch(const View& view, int level, float x, float z, float size, float lo, float hi);

  gfx::Device* device_;
  TextureCache* textures_;
  const MapData* map_ = nullptr;
  gfx::Pipeline pipeline_, shadowPipeline_;
  gfx::Buffer gridVertices_, gridIndices_;
  uint32_t gridIndexCount_ = 0;
  gfx::Texture heightTexture_, normalMap_, maskLow_, maskHigh_, waterMap_, waterRamp_;
  gfx::Texture albedo_[10], normal_[9];
  int shaderKind_ = 0;  // 0: TTerrain (4 strata), 1: TTerrainXP (8 strata)
  std::vector<Level> levels_;
  std::vector<float> ranges_;  // per level: the distance up to which it is used
  std::vector<Patch> patches_;
};

}  // namespace render
