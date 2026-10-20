// Ground cover near the camera (World::groundCover; the modern look): grass blades made by the shader alone, no
// art and nothing kept on the processor. A square of cells round what the camera looks at, each cell a blade or
// none; a cell's place, the blade's lean, height and hue follow from the cell's whole-number place in the world, so
// the pattern stays with the ground as the camera moves. How grassy the ground is and its colour come from
// Terrain::GroundColours (rgb the colour, a how grassy, by the layers' colours); the blade is coloured from the
// ground under it, a share of them drier toward the tip, and sways in the wind the trees sway in. Low (no taller
// than half a small unit's hull, for readability), thinner and gone with the distance; nothing when the camera is
// far, where a blade would be less than a pixel. [cover] (Chris's playthrough of 19 October: "too fake") The tufts
// gathered in patches with gaps between, taller and shorter grass in patches, the blades' colour the meadow's (never the
// brown of bare earth, varied in patches), each blade folded along its middle; none where the ground is bare (`bare`:
// the streams, the trails); the cover thinning out toward `farthest` and coming in over a second (no pop-in).
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "cover_bare.h"  // [cover]
#include "deep_tracks.h"
#include "gfx.h"
#include "json.h"
#include "plants.h"  // [plants]
#include "textures.h"
#include "view.h"

namespace render {

class GroundCover {
 public:
  explicit GroundCover(gfx::Device* device) : bareMap(device), device_(device) {}
  ~GroundCover();
  bool Init(std::string* error);
  // Before the scene's pass (it dispatches): where the grass lies this frame, round what `camera` looks at, and
  // what presses it flat there: `discs` x, z, radius each (world units), what stands on the ground. Nothing when
  // the camera is far.
  void Prepare(const View& view, const Camera& camera, const std::vector<float>& discs);
  // (deep tracks, deep_tracks.h: their field and where its levels are, set by the host before Prepare; none: off)
  // The grass lies flat along them as round what stands.
  gfx::Texture tracks;
  TrackFieldConstants tracksField;
  // Into the scene's pass, after the ground. `heights`: the ground's heights (Terrain::HeightTexture), `heightUnit`
  // world units of its largest value; `cover`: Terrain::GroundColours; `normals`: Terrain::NormalMap; the map's
  // size; `waterLevel`: none at or under it (a map without water: very low). `densityA`, `densityB`: where the
  // plants grow (Terrain::CoverDensity; none: the blades by how grassy the ground is, no plants).
  // `blasts`: the blasts' waves over the square (blasts.h, Blasts::Field, made for Area): the blades and the plants
  // pressed flat away from a blast as its wave passes; none: as without them.
  void Draw(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover, gfx::Texture normals,
            float mapWidth, float mapHeight, float waterLevel, gfx::Texture densityA = {}, gfx::Texture densityB = {},
            gfx::Texture blasts = {});
  // Prepare's square (its first cell's x, z; a cell's size; cells a side), for what is laid over it (the blasts' waves).
  const float* Area() const { return area_; }
  // [tarmac] Where a remastered pad lies over Prepare's square (Tarmacs::ClearCover: R8, its coverage, 1 all), set by the
  // host after Prepare; none: nothing cleared. A blade's tuft and a plant's spot there are left out by it (the worn rim
  // thinned).
  gfx::Texture cleared;
  // [cover] Where the ground is bare over the whole map (bareMap's picture, CoverBare::Picture: R8, 1 bare): a stream's
  // water and its wet bank, the trails and bare patches through the meadow. Set by the host before Draw; none: nothing
  // kept bare. A blade's tuft and a plant's spot there are left out by it, and lower where it thins out.
  CoverBare bareMap;
  gfx::Texture bare;
  // [cover] The cover thins out as the camera goes farther (from `fadeFrom` of `farthest` to nothing at it: each tuft and
  // plant at a distance of its own, growing out of the ground rather than appearing), and comes in over `appearSeconds`
  // when it is first drawn (a map opened, the cover back after the camera was far): no sudden appearance.
  float fadeFrom = 0.7f;
  float appearSeconds = 1.0f;
  // The shaders Draw takes with `blasts` (the same as without, with the waves in them), made the first time the world
  // wants them (after Init); false: they cannot be made (then Draw's `blasts` is left unused).
  bool PrepareBlasts();
  // [plants] Plants that react (plants.h; World::plantsReact), set by the world each frame (null: off): the blades and the
  // plants laid over where units drove and rising again, burnt where the ground is; drawn then by shaders of their own
  // (made by PreparePlants the first time it is wanted, after Init; the blasts' waves in them too).
  const PlantLife* plants = nullptr;
  bool PreparePlants();  // false: they cannot be made (then nothing reacts)

  // The plants on cards beside the blades (the visuals track's atlas for the map, Terrain::Cover: its cells, each
  // a plant drawn side on, its foot at the cell's bottom middle, its size in metres, how it sways, how many make a
  // clump, its group): each cell of a square a little coarser than the blades' a clump of grass or weeds, a flower,
  // dry stalks, a fern or nothing, by the map's table (Terrain::CoverDensity: the layers' rows, flowers in loose
  // patches, more or fewer near trees). [cover] Each plant three cards crossing at its foot, upright and turned its own
  // way in the world (a star: from every side and every height of the camera some of it faces the eye, and it keeps its
  // place and shape as the camera moves round it); the grass's clumps (the "clump" group) are tufts of real blades
  // instead, coloured as the meadow's (a side-on picture of a clump seen from above reads as a flat strip on the
  // ground: Chris, 2026-10-19). Scaled so the tallest is `plantTallest` (no taller than half a small unit: readability)
  // and the low ones `plantBoost` times their real proportion to it, up to that cap (else they would be too small to
  // read from the game's camera), pressed flat round units as the blades are, swaying in the same wind (more toward
  // the top), lit as the ground a half toward the card's own normals (the side the eye sees), the sun's shadow taken
  // at each corner. `folder`: what the atlas's paths are below (kRemasteredTerrain). False where the cover has no
  // usable cells (then no plants).
  bool SetPlants(const JsonValue& cover, TextureCache* textures, const std::string& folder);
  float plantCell = 0.3f;     // one spot (a clump, a flower, ...) or none in a cell this wide
  float plantTallest = 0.24f; // world units: the tallest plant's height
  // spots a square world unit where the table says 1 (Chris, 2026-10-17, having seen them at 15: on; 0 none)
  float plantDensity = 15;
  float plantBoost = 2.75f;   // the low plants this many times their real proportion to the tallest, up to the cap
  // [coverdark] how far the grass's clumps (the cells of the cover's "clump" group but its dry ones) take the colour of the
  // ground under them, as the blades do (0: the art's own; their mean from the cover's stats, none without)
  float clumpsToGround = 1;
  // [cover] how far the weeds and ferns (dark leaves) are brought toward the brightness of the ground under them (0: the
  // art's own; their mean from the cover's stats): at the art's own they read as black holes in the shade of the trees
  float leavesToGround = 0.5f;
  int drawnPlants = 0;        // (the last Draw: spots gone through)

  float bladeHeight = 0.22f;  // the tallest blade, world units
  float bladeWidth = 0.025f;  // at its foot
  float cell = 0.15f;         // one tuft of three blades (or none) in a cell this wide
  float farthest = 50;        // no grass at all when the camera is farther from what it looks at (a blade is then
                              // less than a pixel)

  int drawnBlades = 0;  // (the last Draw: cells gone through)

 private:
  // (the plants' constants as the shader reads them, kept from SetPlants: per cell its uv rectangle; its frame, low
  // and high height, height (metres); its sway, clump's least and most, group; the cells by group, and per group
  // where its list starts and how long it is: 0 grass, 1 dry grass, 2 weeds, 3 flowers, 4 stalks, 5 ferns)
  struct PlantData {
    float cells[16][3][4] = {};
    float order[4][4] = {};
    float groups[6][4] = {};
    float tones[16][4] = {};  // [coverdark] per cell: its mean colour (linear), 1 where it takes the ground's ([cover] -1:
                              // its brightness toward the ground's: the weeds and ferns)
    float tallest = 1;  // the tallest plant's real height (metres)
    float patchScale = 12, patchCover = 0.35f;
    int count = 0;
  } plants_;
  gfx::Texture atlas_, atlasNormals_;
  gfx::Pipeline plantPipeline_;
  // (the grass and the plants with the blasts' waves: Draw's `blasts`, PrepareBlasts's; -1 they cannot be made)
  gfx::Pipeline blastPipeline_, blastPlantPipeline_;
  gfx::PipelineDesc blastDesc_;
  std::string blastSource_, blastPlantSource_;
  int blastsMade_ = 0;
  // [plants] (the grass, the plants; the same two with the blasts' waves: PreparePlants's)
  gfx::Pipeline reactingPipelines_[4];
  std::string reactingSources_[4];
  int reactingMade_ = 0;
  gfx::Device* device_;
  gfx::Pipeline pipeline_, trample_;
  gfx::Texture trampled_;  // (how flat the grass is pressed over its square: 1 flat)
  float area_[4] = {};     // (Prepare's square: its first cell's x, z; a cell's size; cells a side)
  bool prepared_ = false;
  // [cover] (how much of the cover is there this frame, Prepare's: by the camera's distance and since it came in; how far
  // it has come in, 0..1; the view's time of the last frame it was prepared)
  float shown_ = 1, cameIn_ = 0, lastShown_ = -1e9f;
};

}  // namespace render
