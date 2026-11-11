// The ground: the map's heightfield drawn with level of detail, textured and lit the way the original
// game does it (its "TTerrain" and "TTerrainXP" looks).
//
// Geometry: the heights live in a texture; the mesh is one small grid of quads drawn many times, each
// time for a square patch of the map at some level of detail (level k: quads of 2^k cells). Which
// patches are drawn at which level is decided per frame from the camera (the scheme known as CDLOD):
// near ground gets small quads, far ground large ones, and within each level the vertices slide
// toward the next coarser grid as distance grows, so levels meet without cracks or popping.
#pragma once
#include <algorithm>  // [shadowtri] (LowestIn)
#include <limits>
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
  // frame rainGround is on), not when the ground first gets wet in a game; [snowhitch] and its matt variant with mattGrazing
  // (the one the ground is drawn with then: 0.2 s kept, 2.3 s on a new build, at the first rain or after a thaw).
  void PrepareWet() {
    if (rainGround && richGround && WetReady() && MorePipeline(1) && mattGrazing) MattVariant(morePipeline_[1]);
  }
  // [zoomhitch] The dry ground's matt variants (mattGrazing) made now, of the pipelines the dry ground is drawn with (the
  // ground's own or its parallax, and the mask's mud where it is made), not the first frame each is drawn: the mud's is first
  // wanted when a patch that may hold mud comes into the view, which zooming out from a base did at some 450-500 away
  // (64-100 ms with the driver's copy kept, Chris's snapshot item 2). World::PrepareAhead calls it on the device's own thread.
  void PrepareMatt() {
    if (!mattGrazing) return;
    const bool relief = parallax && materials_ && richGround && parallaxPipeline_;
    if (pipeline_) MattVariant(relief ? parallaxPipeline_ : pipeline_);
    if (morePipeline_[0]) MattVariant(morePipeline_[0]);
  }
  // [snowhitch] The lying snow made ready now, before the first flake (World::readAhead, each frame while the game's weather
  // can snow: World::SnowAhead; Chris, 2026-10-21: "the game froze for around 5-10 seconds right before it began to snow",
  // 7.7 s in the viewer): the snow materials read with the wet twins into the one set as the first snow read them (once:
  // not a second set over the one the wet ground read the first frame), the ground's snow pipelines (dry, and with the wet
  // twins: a thaw wets the snow) with their matt variants, the shelter's pass and the map's drift mask. What is made is what
  // the snow's first frame would have made: the pictures are the same. Before PrepareWet in a frame.
  void PrepareSnow();
  // [snow ground] The lying snow drawn from the map's snow materials and shaded by what is around it (World::snowGround sets
  // it each frame; the modern look while snow lies). Where the map's entry in remap.json holds "snow": {"fresh": m, "drift": m,
  // "packed": m}, the snow's colour, small shapes, creases, roughness and relief (with the parallax) are the "fresh"
  // material's at its real size in place of the one colour; "drift" where it lies thickest (the slopes facing away from
  // `snowWind` and the ridges, blended in softly by the slope and the snow's depth); "packed" where it is trodden (deep
  // tracks' troughs, the snow tracks); where it thins it breaks up by the ground's and its own height (the hollows first,
  // the ground's stones showing through); at the thaw and in rain wet snow, darker and glossier, the hollows first. On any
  // map, with or without the materials: the snow shaded by what is around it (its shelter, made once: the sky's light cut
  // in valleys and hollows by the map's heights over some 80 world units, beside rocks and trees by the props, under the
  // trees' crowns; ridges a little brighter), lying deeper on slopes away from the wind and in hollows, thinner and
  // patchier under trees, on steep and windward slopes. Drawn by a pipeline of its own ("terrain, snow": GROUND_MORE and
  // GROUND_SNOW; with GROUND_WET while the twins are drawn) over all the ground while snow lies; false, or no snow lying: as
  // before.
  bool snowGround = false;
  // [leftover] Dry ground matt at grazing light (World::groundMatt sets it each frame; the modern look): the sun's (or the
  // moon's) highlight on the ground fades as the light sinks toward the ground's own slope (from some 60 degrees over it: two
  // thirds left at 35, two fifths at 25, a sixth at 12, a tenth on the horizon), where the ground is dry: a shore's wet band,
  // rain-wet ground, puddles and lying snow keep theirs (lighting.h, GROUND_MATT). The scans' roughness (soil about 0.5, dry
  // grass 0.7) is their grains' as seen from above; with a low light ahead of the eye it lit the soil between the grass blades
  // with a broad sheen that read as wet plastic at dusk and dawn. Drawn by variants of the ground's pipelines (MattVariant);
  // false: as before.
  bool mattGrazing = false;
  float snowWind[2] = {0.915f, 0.403f};  // (the way the wind blows, x and z: World's)
  // (each 0..2, 1 as tuned: how much the shelter cuts the sky's light, how much the trees' crowns take of the snow and its
  // light, how high the wind's drifts stand, how deep they lie and how much of them is drifted; the viewer's --snow-ground-tune)
  float snowTune[4] = {1, 1, 1, 1};
  // [snowhold] How much more snow the steep, windward and scoured ground holds than this ground's own snow lays there (0: as
  // before; 1: as the plain snow, nothing thinned; Chris, 2026-11-04: "maybe halfway"): viewer --snow-hold.
  float snowHold = 0.25f;  // (measured on SETONS_R's west hill: its face 52 % bare at 0, 17 % at 0.5 and in the plain snow; 32 % at 0.25, halfway)
  // [snow mask] How much the map's painted drift mask (remap.json's "snow_drift_mask", the visuals session's: 0.5 as
  // elsewhere, above deeper, below scoured) moves the snow's laying in its shelter, added to the wind's and the props' own
  // (1 as made; 0 none; the viewer's --snow-mask-strength). Taken when the shelter is made.
  float snowMaskStrength = 1;
  // [snow ground] The snow's shelter over the map, made the first time it is wanted while snow lies (it dispatches: between
  // passes; World::Render): `props` (snow_ground.h: the map's props laid over the map at the shelter's size, World makes
  // it). Made again when the mud mask comes or goes (it holds the mask's copy). False: it cannot be made (the snow is drawn
  // as without the option).
  bool SnowShelter(gfx::Texture props);
  // [decalsnow] The shelter while the ground's last drawing (not the sun's) laid the snow from it, else none; and whether it
  // drew the snow's materials then (their wider, mottled margin). The decals lay the same snow (decals.h, Decals::snowGround).
  gfx::Texture SnowShelterLaid() const { return snowLaidNow_ ? shelter_ : gfx::Texture{}; }
  bool SnowMaterialsLaid() const { return snowLaidNow_ && snowMaterialsNow_; }
  bool SnowShelterWanted();  // (whether it is to be made now: none yet, or the mud mask came or went)
  // [leftover] The shelter as made (none: not yet, or it cannot be), for what else lies under the snow as the ground's snow
  // does (the ground cover, grass.h: GroundCover::snowShelter); with snowGround off, none.
  gfx::Texture SnowShelterPicture() const { return snowGround ? shelter_ : gfx::Texture{}; }
  // (the shelter's texels across and down, for World's raster of the props: two a world unit, at most 2048 each way)
  void SnowShelterSize(int* width, int* height) const;
  // Where the kinds of ground lie, over the whole map at half its size (RGBA8: sand, grass, snow, hard ground; the
  // rest dirt and mud), by the layers' kinds blended by their masks. Made the first time it is asked for once a map
  // is loaded (it dispatches: between passes).
  gfx::Texture GroundKinds();
  // The world beyond the map's edge: how far out (world units) the ground goes on, as the map
  // laid out again mirrored on every side, which joins without a seam. 0: the map ends at its
  // edge. The host sets it with the view's `beyond` (World does).
  float beyondReach = 0;
  // (surround) False: the ground beyond the edge is not the map laid out again but the map's own sculpted surround,
  // drawn by the host (surround.h); the map's ground ends at its edge, and beyondReach says how far that other goes.
  bool beyondCopies = true;
  int MaterialLayers() const;  // how many of the map's layers have a third map

  // Builds everything for a map. `map` must stay alive while the terrain is used.
  bool Load(const MapData* map, std::string* error);
  // Into the scene, or (`shadowPass`) into a shadow map, where only depth is written. [shadowmask] `beyondToo`: a depth
  // pass with the ground beyond the map's edge too, as the scene has it (the camera's depth for the sun's shadow mask,
  // sun_mask.h).
  void Draw(const View& view, bool shadowPass = false, bool beyondToo = false);
  // [shadowmask] The sun's shadow mask as the ground reads it (sun_mask.h, SunMask::Rows): bound at the shader buffers' slot 0
  // for the scene's pass while set (the world sets it each frame; none: not bound, the mask not read).
  gfx::Buffer sunMaskRows;
  // The map's heights of the grid points [x0, x1] x [z0, z1] have changed (the simulation laid the
  // ground under a structure flat): drawn so from now on. Within a frame.
  // [terraform] `atOpen`: the heights as the map's file has them (the caller's copy, as large as the map's), where it has
  // them: the ground's stored normals then follow the change too (NormalsChanged, below). Without them the normals stay.
  void HeightsChanged(int x0, int z0, int x1, int z1, const std::vector<uint16_t>* atOpen = nullptr);
  // [terraform] The ground is lit, its sun masked, its rays bent and its grass leant by the map file's stored normal map
  // (NormalMap), not by its heights: where the game changed the heights (a hillside laid flat for a factory, a script's
  // terraforming) the ground was flat to the eye's outline and still shaded as the old slope, for the rest of the game
  // (review of 2026-11-09, ground 1). With this on (the default) the stored normals follow: where a grid point's height, or
  // one of its four neighbours', is no longer the file's, the normal there is worked out from the heights as they are now
  // (central differences, packed as the file packs its own: x in alpha, z in green), fading into the file's over one cell
  // round such points; everywhere else the file's own blocks stay, bit for bit (they hold finer relief than the heights
  // give: on SETONS_R four texels a cell). Heights that go back to the file's take the file's normals back (a game begun
  // again). Only a map whose normal map is one level of DXT5 with sides of whole blocks (every stock map's, SETONS_R's);
  // any other keeps its normals as they are, said once. False: the stored normals stay the file's, as before.
  bool normalsFollow = true;
  int normalBlocksChanged = 0;  // (the texture's 4x4 blocks that are not the file's at this moment: the viewer's last line)

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
  // [cardcost] The ground's lowest point now (world height; the coarsest nodes' lowest): -1e9 before a map is loaded.
  float Lowest() const {
    if (levels_.empty() || levels_.back().lo.empty()) return -1e9f;
    float lowest = levels_.back().lo[0];
    for (float lo : levels_.back().lo) lowest = lo < lowest ? lo : lowest;
    return lowest;
  }
  // [shadowtri] The ground's lowest point under a box of the world (x0..x1, z0..z1; world height): the lowest of the nodes
  // it touches, at the finest level where it touches at most 16 each way (a few hundred nodes looked at, at most; a node
  // holds the points on its edges, so the box's own are in). The ground beyond the map's edge is the map mirrored, one copy
  // each way as drawn (InWorld): a box reaching into a copy takes the part of the map that copy's piece mirrors (the map
  // from its edge in as far as the box reaches out, a little more than is needed). A box reaching past those copies, an
  // empty one, or no map: the map's lowest (Lowest), as before. Shadows::fitLocalFloor's floor.
  float LowestIn(float x0, float z0, float x1, float z1) const {
    if (levels_.empty() || !map_ || !(x0 <= x1 && z0 <= z1)) return Lowest();
    const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
    if (!(x0 >= -w && z0 >= -h && x1 <= 2 * w && z1 <= 2 * h)) return Lowest();
    // (folded onto the map: a..b of a side `size` long, to the part of 0..size it and its mirror images cover)
    auto fold = [](float a, float b, float size, float* lo, float* hi) {
      *lo = std::max(a, 0.0f);
      *hi = std::min(b, size);
      if (a < 0) *lo = 0, *hi = std::max(*hi, std::min(-a, size));
      if (b > size) *hi = size, *lo = std::min(*lo, std::max(2 * size - b, 0.0f));
    };
    float fx0, fx1, fz0, fz1;
    fold(x0, x1, w, &fx0, &fx1);
    fold(z0, z1, h, &fz0, &fz1);
    size_t k = 0;
    while (k + 1 < levels_.size() && std::max(fx1 - fx0, fz1 - fz0) > 16.0f * static_cast<float>(levels_[k].nodeCells)) ++k;
    const Level& L = levels_[k];
    const float cells = static_cast<float>(L.nodeCells);
    const int nx0 = std::min(static_cast<int>(fx0 / cells), L.nx - 1), nx1 = std::min(static_cast<int>(fx1 / cells), L.nx - 1);
    const int nz0 = std::min(static_cast<int>(fz0 / cells), L.nz - 1), nz1 = std::min(static_cast<int>(fz1 / cells), L.nz - 1);
    float lowest = std::numeric_limits<float>::max();
    for (int nz = nz0; nz <= nz1; ++nz)
      for (int nx = nx0; nx <= nx1; ++nx) lowest = std::min(lowest, L.lo[static_cast<size_t>(nz) * static_cast<size_t>(L.nx) + static_cast<size_t>(nx)]);
    return lowest < std::numeric_limits<float>::max() ? lowest : Lowest();
  }
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
  // [terraform] the file's normal map where it can follow the heights (normalsFollow): its blocks (they lie in the map's
  // data, MapData::normalMap, which outlives the terrain), its size in texels, and which of the texture's blocks are no
  // longer the file's (a byte a block; made at the first change)
  const uint8_t* normalBlocks_ = nullptr;
  int normalWidth_ = 0, normalHeight_ = 0;
  std::vector<uint8_t> normalDirty_;
  bool normalsSaid_ = false;
  void NormalsChanged(int x0, int z0, int x1, int z1, const std::vector<uint16_t>& atOpen);

 public:
  // The ground's normals (x in alpha, z in green, as the terrain's shader reads them), for others (ray_scene.h).
  gfx::Texture NormalMap() const { return normalMap_; }
  // The ground's colour for what does not draw it (the ray tracing scene, ray_scene.h): over the whole map at half
  // its size (RGBA8, as stored: the shaders make it linear), each layer by its texture's average laid over the
  // others as the shader lays them, the upper layer at a level that suits the picture. Made the first time it is
  // asked for once a map is loaded (it dispatches: between passes). Its alpha: how grassy the ground is there (by how
  // green each layer's average is; the ground cover reads it).
  gfx::Texture GroundColours();
  // (surround) The layers' own pictures for what draws ground as the map's beside it (the map's surround, surround.h):
  // bound where the ground's own shader has them (t2, t3 the two masks; t6 to t15 the ten albedos; t16 to t24 the nine
  // normals; t25 the layers' third maps, where the map has them), and what the ground's constants say of each layer
  // (`albedoTile` x: its repeats a world unit, y: 1 = it has its third map; `normalTile` x: its repeats a world unit, y:
  // 1 = +y is up in its texture), `xp` 1 for the look with eight strata, the map's specular colour. False before a map
  // is loaded.
  struct LayerTiles {
    float albedoTile[10][4] = {};
    float normalTile[9][4] = {};
    float xp = 0, specular = 0;
  };
  bool BindLayers(LayerTiles* tiles) const;
  // (surround) The upper layer (the tenth: one picture over the whole map, laid over all the others by its alpha) where
  // each of the nine layers under it shows, on the whole: `out[n]` its mean colour (as stored) and its mean alpha over
  // the map, weighted by how much of layer n shows at each place (the masks' shares, laid as the shader lays them, squared); a
  // layer that shows nowhere gets the picture's mean. For ground of those layers past the map's edge, where that
  // picture does not reach. False (and all of `out` 0) where the map has no upper layer, or it or the masks cannot be
  // read on the processor.
  // ([surahead] `upperFile`: the upper layer's file's bytes, where the caller has read them: then nothing but the open
  // map is read here, and it may be called from another thread once the map is loaded)
  bool UpperByLayer(float out[9][4], const std::string* upperFile = nullptr) const;
  // (surround) [snowsur] The lying snow as the ground's last scene drawing laid it (Draw, by its snow pipeline), for
  // what lays snow as the ground does beside it (the map's surround, surround.h): its constants as the ground's shader
  // has them (snowInfo, snowSlice, snowWind, snowTune), the snow's normals and thin cover bound at `normalsSlot` (the
  // ground's wetNormalMaps) and the snow's shelter over the map at `shelterSlot`; the snow's albedos and third maps are
  // layers of the layers' third maps' texture (BindLayers binds it, t25). False while the ground lays no snow that way.
  struct SnowLook {
    float info[4] = {}, slice[4][4] = {}, wind[4] = {}, tune[4] = {};
  };
  bool BindSnow(SnowLook* look, int normalsSlot, int shelterSlot) const;
  // The statics lighting.h's snow reads under GROUND_SNOW, as the ground's own source declares them before the
  // lighting's block (with the deep tracks' statics lighting.h's snow reads beside them, all none).
  static std::string SnowStatics();
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
  // third map's layers of materials_, made again with them after the layers' own third maps, the normal's of wetNormals_
  // ([snow ground] which holds the snow's normals too, after the twins'); -1 none); the layers' own third maps' paths
  // (Load), for that
  std::string wetFiles_[9][3];
  float wetSoak_[9] = {}, wetScale_[9] = {};
  int wetSlice_[9][3] = {};
  gfx::Texture wetNormals_;
  bool wetTried_ = false, wetAny_ = false;
  std::vector<std::string> arhPaths_;
  bool WetReady();
  // [snow ground] the map's snow materials (Remaster: its entry's "snow"; fresh, drift, packed), each its albedo, normal and
  // third map's files and its real size (m); where they lie once read (SnowReady: the albedo's and the third map's layers
  // of materials_, the normal's of wetNormals_; -1 none); the snow's repeat (world units) and relief
  std::string snowFiles_[3][3];
  float snowMetres_[3] = {};
  int snowSlice_[3][3] = {};
  // [snowthin] the map's thin snow's cover (remap.json's snow "thin_cover", tiling with its "thin" material): where the snow
  // lies first as it builds up (1) and where the ground shows longest (0); its file, its repeat's real size (metres), and
  // its layer of wetNormals_ (-1: none)
  std::string thinCoverFile_;
  float thinMetres_ = 0;
  int thinSlice_ = -1;
  bool snowTried_ = false, snowAny_ = false;
  bool SnowReady();
  float SnowSize(int k) const;
  // [snow ground] [rain ground] materials_ made again with the layers' third maps, then the wet twins (`wet`) and the snow's
  // materials (`snow`) after them, and wetNormals_ with their normals (both sets' slices found again); what it holds now
  bool ReadExtras(bool wet, bool snow);
  bool extrasWet_ = false, extrasSnow_ = false;
  // [snow ground] the ground's pipelines with the snow (GROUND_SNOW; [1] with the wet twins too; each made the first time), the
  // shelter, its pass and what it was made with
  gfx::Pipeline snowPipeline_[2];
  bool snowPipelineTried_[2] = {};
  bool SnowPipeline(int wet);
  // [leftover] (mattGrazing) each of the ground's pipelines' variant with GROUND_MATT, made the first time it is wanted: the
  // plain, the parallax, the two mud, the two snow ones; the pipeline itself where its variant cannot be made
  gfx::Pipeline mattPipeline_[6];
  bool mattTried_[6] = {};
  gfx::Pipeline MattVariant(gfx::Pipeline of);
  gfx::Pipeline shelterPass_;
  gfx::Texture shelter_;
  bool shelterTried_ = false, shelterMud_ = false;
  bool ShelterPass();  // [snowhitch] (made the first time: SnowShelter, PrepareSnow)
  void ReadSnowMask();  // [snowhitch] (snowMask_ read the first time: SnowShelter, PrepareSnow)
  // [snow mask] the map's drift mask: its file (Remaster: the entry's "snow_drift_mask"), its texture (read the first time the
  // shelter is made with it; a mask that cannot be read: the shelter as without it)
  std::string snowMaskFile_;
  gfx::Texture snowMask_;
  bool snowMaskTried_ = false;
  bool snowLaidNow_ = false, snowMaterialsNow_ = false;  // [decalsnow] (the last scene drawing's: SnowShelterLaid)
  SnowLook snowLast_;  // [snowsur] (the last scene drawing's snow constants: BindSnow)
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
