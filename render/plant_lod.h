// [plantlod] The ground cover's plants by their size on screen (GroundCover::plantsFar, World::plantsFar; Chris's test run
// of 21 October: "at a distance they are low res like units then close up they are higher res so it just doesn't
// appear", and the ferns gone at the farther of his two zooms).
//
// Without it the plants are drawn over a square round what the camera looks at, and all of them vanish when the camera
// is more than GroundCover::farthest (50) from it, while a fern is still some twenty pixels across. With it:
//  - The plants are drawn over all the ground in view, in tiles of 16 x 16 of the plants' cells (PlantTiles: a tree of
//    squares over the map, each kept where it is in view, above the water and near enough for its largest plant to be
//    seen; each frame on the processor). A tile says how many corners its spots need: a clump's plants all of them
//    while it is large on screen, one plant (18 corners) once it is small.
//  - Each spot is drawn while its card is at least a few pixels across (PlantLodTune::cutLow .. cutHigh, each spot at a
//    size of its own in that band, shrinking into the ground as it nears it: no ring where the plants end). A clump of
//    several plants becomes one larger plant as it gets small on screen (collapseHigh .. collapseLow: the others
//    shrinking away while the first grows, its share of the ground kept).
//  - Where they are no longer drawn, their colour and share of the ground is laid on the ground itself (the far cover,
//    GroundCover::DrawFarCover: a pass over the ground's pixels, by the cover's own table), as are the meadow's blades
//    beyond their square, so that the ground keeps its look when the camera zooms out.
//  - The visuals session's 3D plants (the cover json's "models", PlantModels): the plants of a card that has a model are
//    drawn as that model at the level their size on screen asks for (PlantLodTune::modelPixels), the card below the last
//    level. Placed on the card (a compute pass over the near tiles, the same placement as the cards': which spot, which
//    plant, its foot, height and turn), so a plant keeps its place and size as it changes level.
// The sizes on screen are measured as the models' are (mesh_renderer.cpp, Consider: by the camera's LOD scale too).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gfx.h"
#include "json.h"
#include "linalg.h"
#include "textures.h"
#include "view.h"

namespace render {

// The sizes on screen the plants change at (pixels across of a plant's card, its own width at its height).
struct PlantLodTune {
  float collapseHigh = 40, collapseLow = 20;  // a clump's plants all drawn above the first; one plant below the second
  // each spot drawn down to its own size in this band (pixels at 1440 lines; a taller picture: as many times more, so
  // that a 4K screen does not draw four times as many)
  float cutHigh = 12, cutLow = 6;
  float farCover = 1;  // how much of their colour the blades and plants no longer drawn lay on the ground (0: none)
  // and of that the plants' (the blades' all of it). Measured (2026-10-21, plants drawn against none at 70 world units):
  // the plants drawn change the ground's mean colour by about nothing (they are tinted toward it), while their far colour
  // as worked out here brightened it; so none by default (the blades' alone), there to try
  float farPlants = 0;
  // the models' levels: level 0 above the first, 1 above the second, 2 above the third, the card below (each plant at its
  // own sizes within `modelBand` of them: no ring where they change)
  float modelPixels[3] = {220, 80, 30};
  float modelBand = 0.3f;
  // for pictures: a quarter of the spots, wherever the table lets anything grow, this cell (its index in the cover; a
  // model with no card: 16 and on, in the order of the cover's models); -1: as the table says
  int onlyCell = -1;
  // the plants beyond the square cleared where the near ones are (the far field: pads, what stands); 0 not (to compare)
  float farClear = 1;
};

// The tiles of plants over the view this frame (GroundCover::plantsFar).
class PlantTiles {
 public:
  // The ground's heights (the heightfield's points, (width + 1) x (height + 1)) and the water's level: kept as the least
  // and most height over squares, for which tiles stand in view.
  void SetHeights(const std::vector<uint16_t>& heights, int width, int height, float heightScale, float waterLevel);
  struct CellSize {
    float frame = 0;     // its card's width at its own height, world units (0: no such cell)
    int clumpMost = 1;   // the most plants a spot of it has
    bool model = false;  // a model draws it while it is large enough on screen
    float modelDownTo = 0;  // (a model's: the pixels down to which it may be one)
    bool cardless = false;  // a model with no card: never a card (no corners for it)
  };
  // This frame's tiles round `view` for plants on `cell`-wide cells of these sizes (per cell of the cover); `tallest`:
  // the most a plant stands over the ground (world units).
  void Select(const View& view, float cell, const std::vector<CellSize>& cells, const PlantLodTune& tune, float tallest);
  // per tile one float4: its first cell's x and z (whole cells), its row of the models' flags (-1: none), 0
  std::vector<float> rows;
  // per tile of the models' pass (those with a row of flags), the same first cell: x, z, 0, its nearest distance
  std::vector<float> modelRows;
  struct Batch {
    int corners = 18;  // a spot's corners in these tiles (18 a plant)
    int first = 0, count = 0;
  };
  std::vector<Batch> batches;  // the tiles by how many corners their spots need, each a draw
  int Count() const { return static_cast<int>(rows.size() / 4); }
  float focal = 1;   // pixels a world unit at a distance of one (the camera's LOD scale in it)
  float cutScale = 1;  // the cut's pixels by the picture's height (PlantLodTune::cutLow)
  static constexpr int kSide = 16;     // cells along a tile's side
  static constexpr int kMost = 16384;  // tiles at most
  static constexpr int kMostModelTiles = 2048;

 private:
  int width_ = 0, height_ = 0;  // the map's
  float water_ = -1e9f;
  // the least and most height over squares of 4 world units, then 8, 16, ... (level 0 first)
  struct Level {
    int w = 0, h = 0, size = 4;
    std::vector<float> low, high;
  };
  std::vector<Level> levels_;
  void Range(float x0, float z0, float x1, float z1, float* low, float* high) const;
  void Visit(const View& view, int tx, int tz, int span, int tilesX, int tilesZ, float tileWide, float cell,
             const std::vector<CellSize>& cells, const PlantLodTune& tune, float tallest, float reach);
  struct Found {
    int x, z, corners;
    bool model;
    float dNear;
    float rank = 0;  // [nightlight] (dNear, a tenth nearer where it was among them the frame before)
  };
  std::vector<Found> found_;
  // [nightlight] the tiles kept and those given rows of flags the frame before (x | z << 16, sorted): where there are more
  // than kMost, or more with models than kMostModelTiles, the nearest are taken, those of the frame before counted a tenth
  // nearer, so that a tile at the edge does not switch in and out (its plants popping) as the camera moves a little
  std::vector<uint32_t> lastTiles_, lastModelTiles_;
  int debugCalls_ = 0, debugSwitched_ = 0, debugModelWanted_ = 0, debugFound_ = 0;
};

// The visuals session's 3D plants for the cover (its json's "models": per kind its levels' meshes, SCM v5 in metres, its
// base at height 0, upright, one bone; its own textures as a prop's - LabPBRAlpha: albedo with its coverage in alpha,
// normals in FAF's layout, the material, the ambient occlusion; the card it ends on; its size in metres; optionally the
// distances it changes level at). GroundCover places and draws them.
class PlantModels {
 public:
  explicit PlantModels(gfx::Device* device) : device_(device) {}
  ~PlantModels();
  // Reads the cover's models (their files below `folder`); `cellIndex` gives the index of a card by its name (-1: none).
  // False where none can be drawn.
  bool Set(const JsonValue& cover, TextureCache* textures, const std::string& folder, const std::vector<std::string>& cellNames);
  static constexpr int kMostKinds = 8;
  static constexpr int kLevels = 4;  // (three and the card it ends on, or a fourth level of its own)
  struct Level {
    gfx::Buffer vertices, indices;
    uint32_t indexCount = 0, indexBytes = 2;
    float height = 1;  // the mesh's height (model units, metres)
  };
  struct Kind {
    std::string name;
    int card = -1;  // the cover's cell it ends on (-1: none, "card": null; then its last level down to the cut)
    // a kind with no card: which group's spots it takes and what share of them (its json's "place": {"group", "share"};
    // without: the ferns', a third: undergrowth where the ferns grow)
    int placeGroup = 5;
    float placeShare = 0.3f;
    float sway = 0.6f;
    int clump[2] = {1, 1};
    Level levels[kLevels];
    int levelCount = 0;
    gfx::Texture albedo, normal, spec, ao;
    float size[3] = {1, 1, 1};  // width, depth, height (metres)
    // its leaves' mean colour, linear (its json's "albedo_lin", or "albedo_srgb8"; none: 0): brought to its card's on screen
    float mean[3] = {0, 0, 0};
    float swap[3] = {0, 0, 0};  // distances (world units) it changes level at, where its json gives them (0: by size)
  };
  std::vector<Kind> kinds;
  int KindOfCell(int cell) const;  // (-1: no model)

  // Room: per kind and level how many plants this frame's draw has room for (the models' pass drops what does not fit
  // to the card). Kept by kind * kLevels + level.
  static constexpr int kRowsABucket = 4;            // rows of the instances' picture a kind's level has
  static constexpr int kInstancesARow = 2048;       // (two texels each)
  static constexpr int kMostInBucket = kRowsABucket * kInstancesARow;
  gfx::Texture Instances();  // RGBA32F, 4096 x kRowsABucket * buckets (made the first time)
  gfx::Texture Counters();   // R32UI, 64 x 1
  gfx::Texture Flags();      // R32UI, 256 x PlantTiles::kMostModelTiles: per spot of a model tile, its plants the models drew

 private:
  gfx::Device* device_;
  gfx::Texture instances_, counters_, flags_;
};

// (the HLSL the plants' shaders take with plantsFar: the tiles, the spot's placing shared with the models' pass; the
// models' placing, their drawing, and the far cover; plant_lod.cpp)
extern const char kPlantLodBlock[];
extern const char kPlantModelPlace[];
extern const char kPlantModelShader[];
extern const char kFarCoverShader[];
extern const char kFarDiscShader[];

}  // namespace render
