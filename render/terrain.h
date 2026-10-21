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
#include <utility>
#include <vector>

#include "deep_tracks.h"
#include "gfx.h"
#include "json.h"
#include "scmap.h"
#include "seasons.h"  // [seasons]
#include "textures.h"
#include "view.h"

namespace render {

class Terrain {
 public:
  Terrain(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  // The richer ground of a remastered map (modern look): each layer's own roughness and crease
  // shading, and layers that meet by their height (pebbles stand out of sand, grass creeps into
  // the joints of rock) instead of fading evenly into each other. false: as a stock map is drawn.
  // Without the layers' third maps it changes nothing.
  bool richGround = true;
  // The picture of where lying snow is trodden away (tracks.h), set by the host each frame; none:
  // the snow lies untrodden. (With deep tracks on: their field, deep_tracks.h, and `tracksField` says where its
  // levels are; all zero: off.)
  gfx::Texture tracksMap;
  TrackFieldConstants tracksField;
  // Deep tracks: what kind of ground a layer is, given (before Load) for the layers whose texture's path holds the
  // first ("soil"): the second, a kind as remap.json's "ground" says it (snow, mud, sand, grass, dirt, hard, or two
  // with a plus). The viewer's --ground-kind; otherwise by remap.json, else by the layer's name.
  std::vector<std::pair<std::string, std::string>> groundKinds;
  // [mud ruts] Deep tracks' mud: where the map's remap.json has a mud material (the map's entry's "mud", else
  // "<its layers' materials' first word>_mud", e.g. setonsr_mud) and a layer is mud by its kind, its ruts are drawn in
  // that material (its albedo, normal and third map, at its real size), when deep tracks are on (tracksField). false:
  // the ruts as deep tracks draw them without it.
  bool mudRuts = true;
  // [parallax] The ground's relief close by (World::groundParallax sets it each frame; the modern look): where a layer has
  // its third map, its picture is read where the eye's ray meets the relief that map's height gives it (parallax
  // occlusion), so that close by its stones stand up and hide what lies behind them and its hollows sink, and
  // (`parallaxShadows`, 0..1) its high places shade the hollows from a low sun. Full within `parallaxReach` world units
  // of the eye, gone by half as far again, and wherever the whole relief would cross less than about a pixel. Its depth:
  // the scans' real relief (about 3 cm; remap.json's real_size_m gives a layer's world units a metre) times
  // `parallaxDepth`. The ground's surface stays the relief's middle (what lies on it stays put on the whole). Drawn by a
  // pipeline of its own (the same shader with GROUND_PARALLAX), made the first time; false: as before.
  bool parallax = false;
  float parallaxDepth = 1, parallaxReach = 20, parallaxShadows = 1;
  // [ground mud] The map's painted mud (World::mudMask sets it each frame): where the map's entry in remap.json names a
  // "mud_mask" (a greyscale picture over the whole map: 0 no mud .. 1 all mud), the map's mud material (the ruts' "mud")
  // laid over the layers by the mask's weight, the ground's hollows first, so that a soft weight gives an uneven edge;
  // wet, darker and glossy in rain with puddles gathering in it, paler and cracked as it dries. Deep tracks press into
  // it as mud (GroundKinds counts it as mud), and the plants do not grow on it (CoverDensity). Modern look only; the
  // patches of the ground that may hold mud (a coarse grid from the mask) drawn by a pipeline of its own (the same shader
  // with GROUND_MORE, made the first time), the rest as without it. false, or no mask: as before.
  bool mudMask = false;
  // [rain ground] The rain-wet ground (World::rainGround sets it each frame): where the map's entry holds
  // "weather": {"rain": {"materials": {"<dry material>": "<wet material>"}, "soak": {"<dry material>": n}}}, each layer
  // drawn in a listed material goes over to its wet twin as the weather's wetness rises, its hollows first (by its third
  // map's height), and back as it dries; "soak" says how readily (1: with the wetness; more: sooner). A pixel reads the
  // dry or the wet set, both only on the thin edge between them; the weather's darkening and gloss lie over a twin as over
  // all ground (the visuals session makes each twin the colour of its dry layer as drawn: it changes the make-up, slick
  // mud, not the darkness). Modern look only, the GROUND_WET pipeline while the ground is wet.
  bool rainGround = false;
  // [hitches] The wet ground's pipeline (GROUND_WET) made now where the map has wet twins (World::readAhead: the first
  // frame rainGround is on), not when the ground first gets wet in a game.
  void PrepareWet() {
    if (rainGround && richGround && WetReady()) MorePipeline(1);
  }
  // Where the kinds of ground lie, over the whole map at half its size (RGBA8: sand, grass, snow, hard ground; the
  // rest dirt and mud), by the layers' kinds blended by their masks. Made the first time it is asked for once a map
  // is loaded (it dispatches: between passes).
  gfx::Texture GroundKinds();
  // The world beyond the map's edge: how far out (world units) the ground goes on, as the map
  // laid out again mirrored on every side, which joins without a seam. 0: the map ends at its
  // edge. The host sets it with the view's `beyond` (World does).
  float beyondReach = 0;
  int MaterialLayers() const;  // how many of the map's layers have a third map

  // Builds everything for a map. `map` must stay alive while the terrain is used.
  bool Load(const MapData* map, std::string* error);
  // Into the scene, or (`shadowPass`) into a shadow map, where only depth is written.
  void Draw(const View& view, bool shadowPass = false);
  // The map's heights of the grid points [x0, x1] x [z0, z1] have changed (the simulation laid the
  // ground under a structure flat): drawn so from now on. Within a frame.
  void HeightsChanged(int x0, int z0, int x1, int z1);

  // On-screen size, in pixels, that one quad of the mesh should have: smaller means more triangles.
  float quadPixels = 6;
  // The remastered ground (remasters.h, kRemasteredTerrain): the open map's path, set before Load, by which its
  // remap.json is looked up; `ultraGround`: its 8k materials in place of the 4k.
  std::string mapPath;
  bool ultraGround = false;
  // [seasons] The season the map is drawn in (seasons.h), set before Load (World::Open does): where the map's entry in
  // remap.json has a block for it, its materials, ground cover and mud in place of summer's (a season's material
  // without the 8k tier at its 4k one); Summer, or a map without the block: as always. What the season changes, its
  // props too (World draws them), is SeasonOf()'s once the map is loaded.
  Season season = Season::Summer;
  const SeasonLook& SeasonOf() const { return seasonLook_; }
  int RemasteredLayers() const { return remasteredLayers_; }
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
  void InWorld(float x0, float z0, float x1, float z1, float lo, float hi, Vec3* a, Vec3* b) const;
  int tileX_ = 0, tileZ_ = 0;  // which copy of the map is being gone through (Draw)

  gfx::Device* device_;
  TextureCache* textures_;
  const MapData* map_ = nullptr;
  gfx::Pipeline pipeline_, shadowPipeline_;
  gfx::Buffer gridVertices_, gridIndices_;
  uint32_t gridIndexCount_ = 0;
  gfx::Texture heightTexture_, normalMap_, maskLow_, maskHigh_, waterMap_, waterRamp_;

 public:
  // The ground's normals (x in alpha, z in green, as the terrain's shader reads them), for others (ray_scene.h).
  gfx::Texture NormalMap() const { return normalMap_; }
  // The ground's colour for what does not draw it (the ray tracing scene, ray_scene.h): over the whole map at half
  // its size (RGBA8, as stored: the shaders make it linear), each layer by its texture's average laid over the
  // others as the shader lays them, the upper layer at a level that suits the picture. Made the first time it is
  // asked for once a map is loaded (it dispatches: between passes). Its alpha: how grassy the ground is there (by how
  // green each layer's average is; the ground cover reads it).
  gfx::Texture GroundColours();
  // The ground's heights as the shader reads them (one per grid point), and the world units of their largest value.
  gfx::Texture HeightTexture() const { return heightTexture_; }
  // The visuals track's plants for this map's ground (remap.json: the map's entry names its "cover", the card atlas's
  // JSON with its density table by the map's layers): the JSON (null where the map has none), its files' paths
  // below kRemasteredTerrain.
  const JsonValue* Cover() const { return hasCover_ ? &cover_ : nullptr; }
  // Where the plants grow, over the whole map ([cover] a texel a world unit, at most 2048 a side), by the cover's table
  // blended as the layers are: `a` RGBA8 (blades, clumps, flowers, dry stalks), `b` RGBA8 (ferns, the share of a clump
  // taken from the dry cells, the share of clumps that are weeds, [cover] the blades as the layers give them before the
  // trees change them: CoverBare's trails); near trees (`trees`: R8 over the whole map, 1 within the table's radius
  // of a tree) as its near_trees says, not on the layers it names; [cover] all of it times 1 - the map's cover mask
  // (CoverMask) where it has one. Made the first time it is asked for once a map is loaded (it dispatches: between
  // passes). False where the map has no cover.
  bool CoverDensity(gfx::Texture trees, gfx::Texture* a, gfx::Texture* b);
  // [cover] The map's cover mask (remap.json: the map's entry's "cover_mask", a greyscale picture over the whole map, 0
  // the cover as its table says .. 1 none: the visuals session paints the streams, trails and roads with soft edges),
  // read the first time it is asked for; none where the map has none or it cannot be read. `coverMaskGiven` (the
  // viewer's --cover-mask, set before a map opens): a picture's path in place of the entry's, for tests.
  gfx::Texture CoverMask();
  std::string coverMaskGiven;
  float CoverTreeRadius() const { return coverTreeRadius_; }  // (the table's near_trees radius, world units)
  float HeightUnit() const { return map_ ? 65535.0f * map_->heightScale : 1.0f; }
  // [unit dirt] Layer i's kind of ground (0..9: shares of snow, mud, sand, grass, dirt, hard), as GroundKinds lays them
  // (unit_wear.h: what the units drive through, on the processor).
  const float* LayerKind(int i) const { return groundKind_[i]; }

  // Which of the map's own decals remap.json's entry for the map asks to be drawn ("decals": all, normals, albedo
  // or off; World::Options::mapDecals); empty where it says nothing (Load reads it).
  const std::string& MapDecalsGiven() const { return mapDecalsGiven_; }

 private:
  std::string mapDecalsGiven_;
  gfx::Texture albedo_[10], normal_[9];
  float albedoScale_[10] = {}, normalScale_[9] = {};  // (the layers' scales, the remastered ground's applied)
  bool normalUp_[9] = {};    // (a remastered material's normal: +y up in its texture, the stock layers' turned)
  int remasteredLayers_ = 0;
  SeasonLook seasonLook_;  // [seasons] (Remaster: the open map's, for `season`)
  void Remaster(const MapData& map, std::vector<std::string>* arh);
  void ReadCover(const MapData& map, const std::string& path);
  JsonValue cover_;
  bool hasCover_ = false;
  float coverA_[10][4] = {}, coverB_[10][4] = {};  // (each layer's row of the table: as the two pictures hold it)
  float coverNear_[4] = {1, 1, 1, 1}, coverFern_ = 0, coverTreeRadius_ = 6;
  gfx::Pipeline coverPass_;
  gfx::Texture coverA_tex_, coverB_tex_;
  gfx::Pipeline groundColoursPass_;
  gfx::Texture groundColours_;  // (GroundColours, made again for each map)
  // (deep tracks: each layer's kind of ground, as shares of snow, mud, sand, grass, dirt, hard; as remap.json gives
  // it; GroundKinds' pass and picture)
  float groundKind_[10][6] = {};
  std::string groundGiven_[10];
  gfx::Pipeline kindsPass_;
  gfx::Texture kinds_;
  // [mud ruts] the mud material remap.json names for this map (Remaster): its albedo, normal and third map's files (the
  // layers' tier) and its real size (m); each layer's world units a metre (its scale over its material's real size; 0:
  // not known); and what is made of them for this map (FindMud: the world units a repeat, 0 none; the textures, read
  // when first drawn)
  std::string mudFiles_[3];
  float mudMetres_ = 0, layerUnitsPerMetre_[10] = {};
  float mudSize_ = 0;
  gfx::Texture mud_[3];
  bool mudTried_ = false;
  void FindMud();
  // [parallax] the ground's pipeline with its relief (ParallaxPipeline: made the first time it is asked for), a layer's depth
  gfx::Pipeline parallaxPipeline_;
  bool parallaxTried_ = false;
  bool ParallaxPipeline();
  float LayerDepth(int i) const;
  // [ground mud] [rain ground] the ground's pipelines with the mask's mud ([0], GROUND_MORE) and with the wet twins too ([1],
  // GROUND_WET), the relief in both (MorePipeline: each made the first time); the mud material's textures read once for the
  // ruts or the mask (MudReady); the mud's repeat whether or not a layer is mud by its kind (FindMud)
  gfx::Pipeline morePipeline_[2];
  bool moreTried_[2] = {};
  bool MorePipeline(int wet);
  bool MudReady();
  float mudSizeAny_ = 0;
  // [ground mud] the mask's file (remap.json's "mud_mask", Remaster), its texture (MudMaskReady: read the first time it is
  // wanted); what GroundKinds' and CoverDensity's pictures were made with (made again when it changes)
  std::string mudMaskFile_;
  gfx::Texture mudMask_;
  bool mudMaskTried_ = false, kindsMud_ = false, coverMud_ = false;
  // [cover] the cover mask's file (remap.json's "cover_mask", Remaster; or coverMaskGiven), its texture (CoverMask: read the
  // first time it is wanted)
  std::string coverMaskFile_;
  gfx::Texture coverMask_;
  bool coverMaskTried_ = false;
  bool MudMaskReady();
  // [ground mud] where on the map there is mud at all, coarsely (MudMaskReady, from a small level of the mask's file): blocks
  // of `mudBlock_` world units, each 1 where its block or one within two of it has any mud, summed (an integral picture,
  // (mudBlocksW_ + 1) x (mudBlocksH_ + 1)); and of each patch drawn (AddPatch, beside patches_) whether it may hold mud. Only
  // those are drawn by the mud's pipeline: the rest of the ground as without it, paying nothing.
  std::vector<int> mudBlocks_;
  int mudBlocksW_ = 0, mudBlocksH_ = 0;
  float mudBlock_ = 0;
  std::vector<uint8_t> patchMud_;
  bool markMud_ = false;  // (Draw: the patches are marked this time)
  bool MudNear(float x, float z, float size) const;
  // [rain ground] per layer: its wet twin's albedo, normal and third map's files (Remaster; empty: none), how readily it
  // soaks, its repeat (world units); where they lie once read (WetReady, the first time it is wanted: the albedo's and
  // third map's layers of materials_, made again with them after the layers' own third maps, the normal's of wetNormals_;
  // -1 none); the layers' own third maps' paths (Load), for that
  std::string wetFiles_[9][3];
  float wetSoak_[9] = {}, wetScale_[9] = {};
  int wetSlice_[9][3] = {};
  gfx::Texture wetNormals_;
  bool wetTried_ = false, wetAny_ = false;
  std::vector<std::string> arhPaths_;
  bool WetReady();
  // A remastered map's third map per layer (beside `<name>_albedo.dds` a `<name>_arh.dds`: red what
  // its creases let in of the sky's light, green how rough it is, blue how high it stands), all
  // nine as the layers of one texture; `material_[i]`: layer i has one.
  gfx::Texture materials_;
  bool material_[9] = {};
  int shaderKind_ = 0;  // 0: TTerrain (4 strata), 1: TTerrainXP (8 strata), 2: TTerrainGlow (TTerrain, the second stratum glowing)
  std::vector<Level> levels_;
  std::vector<float> ranges_;  // per level: the distance up to which it is used
  std::vector<Patch> patches_;
};

}  // namespace render
