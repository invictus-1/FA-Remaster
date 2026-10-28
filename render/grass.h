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
#include <memory>
#include <string>
#include <vector>

#include "camera.h"
#include "cover_bare.h"  // [cover]
#include "deep_tracks.h"
#include "gfx.h"
#include "json.h"
#include "linalg.h"  // [leftover]
#include "plant_lod.h"  // [plantlod]
#include "plants.h"  // [plants]
#include "textures.h"
#include "view.h"

namespace render {

struct PlantConstants;  // ([plantlod] grass.cpp: the plants' shaders' constants)

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

  // [leftover] Motion (motion.h), for the temporal upscaler and frame generation: after Motion::Build, the blades, the tufts
  // and the plants' cards Draw drew this frame, again, each corner where it was the frame before (the wind at `timeBefore`,
  // Motion::TimeBefore; what pressed it then; the rest as now), by the views without the jitter (`now`, `before`: Motion::Now
  // and Before), into `motion` over `depth` (the scene's, one sample a pixel), which it tests and does not write. It begins
  // a pass of its own. Its shaders made the first time each is wanted.
  void DrawMotion(const View& view, gfx::Texture motion, gfx::Texture depth, const Mat4& now, const Mat4& before, float timeBefore);
  // Only with the camera at most this far from what it looks at (a cinematic camera: its eye this high over the ground):
  // the wind moves a blade's tip by some hundredths of a world unit a second, which from farther is less than a tenth of a
  // pixel a frame, left to the camera's part; drawing it again there would cost some 0.3 ms from the game's usual height.
  float motionFarthest = 20;
  // [leftover] Under lying snow (the view's weather.y; the modern look): the cover goes under it as it lies deeper, each blade,
  // tuft and plant by its own height (the shorter first), what stands out of it bowed and dusted white on its upper side.
  // How deep it lies is the light's lying share over the place (lighting.h) times how much lies, times `snowDepth` (world
  // units, where all of it lies; 0: the cover as without snow, whitened by the light's snow as before); with the snow
  // ground's shelter (`snowShelter`, Terrain::SnowShelterPicture; set by the host before Draw, none: not read) deeper in
  // hollows and the wind's lee, thinner under the trees' crowns and on steep and windward ground, by `snowWind` (the way the
  // wind blows, x and z) and `snowTune` (Terrain::snowTune: its y the crowns', its w the drifts' weight).
  float snowDepth = 0.16f;
  gfx::Texture snowShelter;
  // [plantlod] The ground lays the map's snow materials (Terrain::SnowMaterialsLaid; set by the host before Draw): the snow's
  // margin mottled over the cover as over the ground, the marks and the map's low plants (decals.cpp's MarkSnowLying).
  bool snowMaterials = false;
  float snowWind[2] = {0.915f, 0.403f};
  float snowTune[4] = {1, 1, 1, 1};

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
  // [cover] how much brighter the visuals session made the weeds' and ferns' art after the lift above was tuned (2026-10-21:
  // x1.30 in linear, baked into sorrel, weed, nettle and fern so the lift is not needed over meadow): the lift is worked out
  // so that the screen is as it was (over meadow ground the leaves at the art's own, darker or brighter ground still
  // followed as before). 1: the art as the lift was tuned for.
  float leavesBaked = 1.30f;
  int drawnPlants = 0;        // (the last Draw: spots gone through)

  float bladeHeight = 0.22f;  // the tallest blade, world units
  float bladeWidth = 0.025f;  // at its foot
  float cell = 0.15f;         // one tuft of three blades (or none) in a cell this wide
  float farthest = 50;        // no grass at all when the camera is farther from what it looks at (a blade is then
                              // less than a pixel)

  int drawnBlades = 0;  // (the last Draw: cells gone through)

  // [plantlod] The plants by their size on screen, as the models are (plant_lod.h; Chris's test run of 21 October: "at a
  // distance they are low res like units then close up they are higher res so it just doesn't appear"). Off: as before
  // (a square round what the camera looks at, all of it gone beyond `farthest`). On: the plants over all the ground in
  // view, in tiles chosen each frame (PlantTiles), each spot as long as it is `plantLod.cutLow`..`cutHigh` pixels across
  // on screen (each at its own size in that band, shrinking into the ground), a clump drawn as fewer, larger plants once
  // it is small on screen (`collapseHigh`..`collapseLow`), and where they are no longer drawn their colour laid on the
  // ground (`plantLod.farCover`: DrawFarCover); the blades as before. The world's switch World::plantsFar.
  bool plantsFar = false;
  // [plantlod] Plants with volume from above (World::plantsRound): each plant's cards fanned out from its foot (leaning
  // outward, every one its own way round, as a fern's fronds or a weed's leaves), lit by normals rounded over the plant
  // as a mass, so that from the game's high camera a plant reads as a tuft, not a flat star. Off: upright cards as before.
  bool plantsRound = false;
  // [plantlod] The visuals session's 3D plants (the cover json's "models": each kind its levels and the card it ends
  // on; World::plantModels): with plantsFar on, the plants of a card that has a model drawn as that model at the level
  // its size on screen asks for, the card below the last. Off (or no models in the cover): cards as before.
  bool plantModels = false;
  // [gpuscenery] The 3D plants drawn by indirect draws (gfx.h, Device::IndirectDraws; with MeshRenderer::cardScenery, the
  // viewer's --card-scenery): each kind's level as many instances as its placing put in its list, read by the card, in
  // place of the room guessed for it on the processor (the list's whole room then: no plant of a full list is left a card,
  // and none past the count is put nowhere). Off, as at first: the room guessed, each draw that many.
  bool indirectModels = false;
  PlantLodTune plantLod;  // [plantlod] (its sizes on screen; plant_lod.h)
  // [plantlod] The ground's heights for the tiles (once a map is opened: the heightfield's points, (width + 1) x (height
  // + 1), and world units of a step of them), and the water's level (none under it).
  void SetHeights(const std::vector<uint16_t>& heights, int width, int height, float heightScale, float waterLevel);
  // [plantlod] The cover's 3D plants (its "models" block; SetPlants first): their levels' meshes and their textures read
  // the first time plantModels wants them.
  void SetPlantModels(const JsonValue& cover, TextureCache* textures, const std::string& folder);
  // [plantlod] With plantsFar, before the scene's pass (it dispatches; after Prepare and what is set after it, `cleared`):
  // the tiles over the view this frame, and which plants the models draw (the models' pass's placing).
  void PrepareFar(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover, float mapWidth,
                  float mapHeight, float waterLevel, gfx::Texture densityA, gfx::Texture densityB);
  // [plantlod] With plantsFar, after PrepareFar, between passes: what clears the plants beyond the square round what the
  // camera looks at, as the near cover is cleared - the remastered pads (drawn into FarField over FarArea by the host:
  // Tarmacs::ClearCover, `padsDrawn` whether it drew any) and what stands on the ground (Prepare's discs: buildings' and
  // units' footprints) - into the far field the plants and the far cover read; then the models' placing.
  const float* FarArea() const { return farArea_; }  // (as Area(): its first x, z; a texel's size; texels a side)
  gfx::Texture FarField();                           // (R8, kFarField a side; made the first time)
  void PlaceFar(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, float mapWidth, float mapHeight,
                float waterLevel, bool padsDrawn);
  static constexpr int kFarField = 1024;
  // [plantlod] Modelled kinds with no card of their own stand at most this tall (world units; the cards' plantTallest is
  // for readability among the units; a bush is taller than a fern, as the map's small bushes are)
  float modelTallest = 0.6f;
  // [plantlod] With plantsFar and the far cover on, into the scene's colour after the ground and its marks (a pass of the
  // scene's colour alone: Post::BeginColorOnly), before Draw: where the plants are no longer drawn (too small on screen,
  // or the blades beyond their square), their share of the ground and their colour laid on it, so that zooming out
  // the ground keeps its look (`sceneDepth`: the scene's depth, the ground's alone then).
  void DrawFarCover(const View& view, const Camera& camera, gfx::Texture sceneDepth);
  bool FarCoverWanted() const { return plantsFar && plantLod.farCover > 0 && farReady_; }
  // [plantlod] ([hitches] World::readAhead) The 3D plants' pipelines (drawing and placing; `reacting`: also those with
  // plants that react) made now, before the first plant comes near enough for one: no hitch then.
  void PrepareAhead(bool reacting);
  bool FieldsReady() const { return prepared_; }  // [plantlod] (Area() is this frame's)
  // [plantlod] the far cover's: how much of fully grassy ground the meadow's blades cover seen from above, and how much
  // darker a plant is than its art among its own leaves
  float farBlades = 0.3f, farShade = 0.8f;
  int drawnTiles = 0, drawnModels = 0;  // [plantlod] (the last Draw: tiles of plants, model plants' room)

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
    float cover[16] = {};  // [plantlod] per cell: how much of its frame the picture covers (the cover's stats; the far cover)
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
  // [leftover] the motion's pipelines: [k] the blades (k even) or the plants (odd), + 2 with the blasts' waves, + 4 plants that
  // react (each the scene's own shader with MOTION, made the first time wanted); and what this frame's Draw drew with
  std::string bladeSource_, plantSource_;
  gfx::Pipeline motionPipelines_[8];
  bool motionTried_[8] = {};
  struct Drawn {
    bool blades = false, plants = false, reacting = false, blasts = false;
    bool far = false;  // [plantlod] (the plants by the tiles: DrawFarPlants)
    gfx::Texture heights, cover, normals, densityA, densityB, blastField, cleared, bare, press, burnt, shelter;
  } drawn_;
  std::vector<float> drawnConstants_[2];  // (the blades' and the plants' constants as Draw handed them)
  float away_ = 1e9f;                     // (Prepare's: the camera's distance, or a cinematic eye's height)
  float snowConstants_[8] = {};           // (kCoverShared's CoverSnow, as Draw handed it)
  gfx::Device* device_;
  gfx::Pipeline pipeline_, trample_;
  gfx::Texture trampled_;  // (how flat the grass is pressed over its square: 1 flat)
  float area_[4] = {};     // (Prepare's square: its first cell's x, z; a cell's size; cells a side)
  bool prepared_ = false;
  // [cover] (how much of the cover is there this frame, Prepare's: by the camera's distance and since it came in; how far
  // it has come in, 0..1; the view's time of the last frame it was prepared)
  float shown_ = 1, cameIn_ = 0, lastShown_ = -1e9f;
  // [plantlod] (the plants' pipelines are made for these switches; the cameIn_ part of shown_; the tiles and the models;
  // PrepareFar has set the tiles this frame)
  int plantVariant_ = 0;
  std::string PlantDefines() const;
  void BuildPlantSources();
  void MakePlantVariant();
  float cameInShown_ = 1;
  bool farReady_ = false, modelsPlaced_ = false;
  std::unique_ptr<PlantTiles> tiles_;
  std::unique_ptr<PlantModels> models_;
  gfx::Buffer tileBuffer_, modelTileBuffer_;  // (PlantTiles' rows and model rows, this frame's)
  gfx::Pipeline farCover_, modelPlace_, modelClear_;
  gfx::Pipeline modelPipelines_[8];  // (ModelPipeline's)
  bool modelTried_[8] = {};
  bool modelPlaceTried_ = false;
  int farCoverTried_ = 0;
  uint32_t room_[PlantModels::kMostKinds * PlantModels::kLevels] = {};  // (the models' room this frame, by kind and level)
  float densityMost_[6] = {};  // (per group the most of a cell the cover's table gives it: the models' room)
  // (the far field: its picture, the area it lies over, whether it was made this frame; the discs Prepare was handed; the
  // discs' pipeline)
  gfx::Texture farField_;
  float farArea_[4] = {0, 0, 1, 1};
  int farStep_ = -1;  // [nightlight] the far field's texel, as a step of its ladder (Prepare)
  bool farFieldReady_ = false;
  std::vector<float> farDiscs_;
  gfx::Pipeline farDiscPipeline_;
  bool farDiscsTried_ = false;
  JsonValue modelsCover_;      // (SetPlantModels': read by LoadPlantModels the first time the models are wanted)
  TextureCache* modelsTextures_ = nullptr;
  std::string modelsFolder_;
  bool modelsLoaded_ = false;
  void LoadPlantModels();
  void MakeModelPlace();
  // [gpuscenery] (indirectModels) the lists' draws written on the card after the placing, and whether they were this frame
  gfx::Pipeline modelArgs_;
  gfx::Buffer modelArgsBuffer_;
  bool modelArgsTried_ = false, modelArgsNow_ = false;
  struct Far {
    gfx::Texture cover, densityA, densityB;
    float mapWidth = 0, mapHeight = 0, waterLevel = -1e9f;
  } far_;  // (PrepareFar's, for DrawFarCover)
  int FillPlantConstants(PlantConstants* p, float mapWidth, float mapHeight, float heightUnit, float waterLevel, float pixelAngle, bool reacting) const;
  void DrawFarPlants(PlantConstants* p, int variant, bool motion);
  gfx::Pipeline ModelPipeline(int k);
  float FrameOf(int cell) const;  // (a cell's card's width in world units at its own height, Draw's scale)
  float ModelScale(const PlantModels::Kind& m) const;
  float ModelFrame(const PlantModels::Kind& m) const;
  bool ModelsOn() const;
};

}  // namespace render
