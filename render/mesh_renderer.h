// Draws models: every frame the caller lists what is in the world (a model, where, in which colour);
// each pass then draws what of that list it can see, sorted by mesh, each mesh's instances together.
#pragma once
#include <algorithm>  // [mcrec] (ForEachAddedIn)
#include <utility>    // [step2b] (std::swap)
#include <memory>     // [gpuscenery]
#include <string>
#include <unordered_map>
#include <vector>

#include "card_scenery.h"  // [gpuscenery]
#include "frame.h"
#include "gfx.h"
#include "models.h"
#include "textures.h"
#include "unit_wear.h"
#include "view.h"

namespace render {


// A unit's pose between two beats: its bones that are not at rest at the beat shown and at the one
// after it (each list in rising bone order), and how far the moment drawn is from one to the other.
// The lists must stay where they are until the frame's last Draw.
struct Pose {
  const FrameBone* now = nullptr;
  size_t nowCount = 0;
  const FrameBone* next = nullptr;
  size_t nextCount = 0;
  float blend = 0;
};

class MeshRenderer {
 public:
  MeshRenderer(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  ~MeshRenderer();
  bool Init(std::string* error);

  // Empties the list.
  void Begin();
  // `world` places the model's own units in the world (so it includes the model's scale).
  // `teamColor` is rgb in 0..1; `built` is the fraction complete (1 for a finished unit).
  // `pose`: where its bones are, for a model that does not stand at rest (null: at rest).
  // `tint`: the model is shown as a ghost of that colour (a building's preview while its site is chosen).
  // `built` above 1.5: a finished unit with its personal shield up (a glow along its outline).
  // `treads`: its two treads' scroll (the amounts the original adds to the u of the texture's strips at
  // v above 0.95 and between 0.90 and 0.95); null: none. Not with `tint`.
  // `key`: what it is from frame to frame, for its motion (`motion`): an entity's own number, the same
  // every frame (0: nothing that moves of itself).
  void Add(const Model* model, const Mat4& world, Vec3 teamColor, float built = 1, const Pose* pose = nullptr, const Vec3* tint = nullptr,
           const float* treads = nullptr, uint32_t key = 0);
  // A personal shield's shell (ModelCache::ShieldShell) placed as its unit, lit round these hits: each
  // where it struck on the unit at rest (world units, the unit's own frame) and its age (w, 0..1 of the
  // second it shows). Drawn with what is see-through.
  void AddShell(const Model* shell, const Mat4& world, const std::vector<Vec4>& hits);
  // The battle scars and snow of the model added last (unit_wear.h; World::unitScars, unitSnow): its look this frame,
  // its marks laid in the bone buffer after the shells' hits (Prepare). Without it a model wears nothing of its own.
  void Wear(const WearLook& look);
  // [machinery] The model just added is a factory whose belt has moved `amount` along its strip (Machinery::PoseUnit;
  // below 0: none). Not with a ghost's tint or treads.
  void Belt(float amount);
  // Scenery that stands still (trees, rocks), in groups (one square of the map's): the renderer keeps
  // each group from one frame to the next, so a frame only says which groups it lists (ListGroup,
  // after Begin), not where each tree stands and how large it is. A group is filled anew
  // (ClearGroup, then AddToGroup for each) when something in it has changed. It counts as scenery.
  // The groups' models come first in the frame's list, in the order they are listed.
  uint32_t AddGroup();  // a new, empty group's number
  void ClearGroup(uint32_t group);
  void AddToGroup(uint32_t group, const Model* model, const Mat4& world);
  void ListGroup(uint32_t group) { groupsListed_.push_back(group); }
  // [gpuscenery] Where the world lists a group among the others, always in the same order (World: its square's number):
  // the card draws a level's props of all groups in that order, as the processor's list has them (cardScenery).
  void SetGroupOrder(uint32_t group, uint32_t order) {
    if (group < groups_.size() && groups_[group].order != order) groups_[group].order = order, groups_[group].cardDirty = true;
  }
  // [gpuscenery] The still scenery kept on the card and picked there (card_scenery.h; viewer --card-scenery; off as at
  // first): the groups' props that the card takes are tested, given their levels and listed for the camera's pass, the sun's
  // cascades and the camera's motion of what sways by three compute passes, and drawn by indirect draws, one a level's
  // list; the processor records only those draws. The same picture but where a level is chosen a hair differently in the
  // card's rounding. Where the device cannot (Device::IndirectDraws), and until its shaders' variants are made, as before.
  bool cardScenery = false;
  // [step2b] The card's picking only where it pays (cardScenery on): with few props in view (a close camera) the processor's
  // own tests cost less than the picking's fixed part and the draws of lists that turn out empty, with the whole map in
  // view much more. Each frame counts the props of the groups listed; the card picks from the cardOnFrames-th frame in a
  // row with at least cardOnProps, the processor again from the cardOffFrames-th in a row with fewer than cardOffProps.
  // Both draw the same picture, so the switch, made between frames, shows nothing. Off: the card picks every frame.
  bool cardByNeed = false;
  int cardOnProps = 28000, cardOffProps = 22000, cardOnFrames = 4, cardOffFrames = 30;
  size_t cardListedProps = 0;  // (the last frame's count)
  bool CardNow() const { return cardNow_; }  // (the card picked this frame's scenery)
  float CardPickMs() const;                   // (what its picking took the processor, averaged)
  // After the frame's last Add and before its first Draw: builds the bones of every posed model
  // that one of the frame's views (the camera's, the shadow cascades') will draw, and hands them
  // to the shaders once for all passes.
  void Prepare(const std::vector<const View*>& views);
  // Draws the list as `view` sees it: into the scene, or (`shadowPass`) into a shadow map, where
  // only depth is written. `environment` is the cube map the original look reflects in shiny parts.
  // `glass`: only the see-through levels (ModelLod::glass: shields, frames of light), which the
  // ordinary pass leaves out: after the water, over what is behind them, writing no depth.
  // `part`: 0 everything, 1 the scenery alone (what was added while `addingScenery` was set: trees,
  // rocks), 2 everything else. The scene draws the scenery first, so that what lies on the ground
  // (range rings) can be laid over the trees and under the units, as the original has it.
  void Draw(const View& view, gfx::Texture environment, bool shadowPass = false, bool glass = false, int part = 0);
  bool addingScenery = false;
  // Motion (motion.h). With `motion` on, every model added with a key keeps its place and bones from one
  // frame to the next, and DrawMotion (after the scene's passes) draws those the scene drew where they are,
  // writing where they were the frame before: last frame's place minus this frame's in the picture's 0..1,
  // by the views without the jitter (`now`, `before`). Into a pass with an RG16F colour target and the
  // scene's depth (one sample a pixel), which it tests and does not write.
  bool motion = false;
  // Without motion this frame: last frame's places are kept (not forgotten) for the next frame that has it
  // (a frame drawn for something else in between: World::sidePicture).
  bool keepLast = false;
  // [leftover] `swayMotion`: DrawMotion also draws what sways (trees, bushes: the scenery the scene drew whose level sways)
  // where the wind, the blasts' waves and the plants that react put it this frame, writing where they had it the frame
  // before: the wind at `timeBefore` (the frame before's time; Motion::TimeBefore), the blasts' waves and what pressed it as
  // long before. False: as before (what
  // sways moves with the camera's part alone, and smears under the temporal upscaler).
  bool swayMotion = true;
  // [leftover] Only what stands at least this many pixels tall on the screen (a tree's height, of a group's each tree's): the
  // wind moves a tree's top by about a seventieth of its height, slowly, so a shorter one moves by less than a tenth of a
  // pixel a frame at 60 a second (a twentieth in calm air), which the camera's part leaves unnoticed; while the blasts' waves
  // are under way (blastConstants), which throw trees about fast, the second. (From the game's usual height all the trees in
  // view cost some 0.14 ms.)
  float swayMotionPixels = 128, swayMotionPixelsBlasts = 24;
  void DrawMotion(const View& view, const Mat4& now, const Mat4& before, float timeBefore);
  // Hidden outlines (hidden_outlines.h): a mark (not 0) on the model just added, when it is `model` (Add left out
  // nothing); the scene's pass keeps what it draws of the marked models. DrawMarked, after the scene's passes, draws
  // that again (the same levels) by `view`, with the pipeline set and into the pass under way: those whose mark has
  // `want` in the bits of `mask`, each with its mark's four bytes (the lowest first, 0..255) in its colour's place.
  void MarkLast(const Model* model, uint32_t mark) {
    if (mark && !entries_.empty() && entries_.back().model == model) {
      entries_.back().mark = mark;
      ++marks_;
    }
  }
  void DrawMarked(const View& view, uint32_t mask, uint32_t want);
  size_t MarkedDrawn() const { return markedItems_.size(); }

  // Level of detail is chosen by size on screen, not by distance: a model wishes for
  // `trianglesPerPixel` triangles for every pixel of the circle it covers and gets the level nearest
  // to that wish. So every model costs about the same per pixel, whatever its budgets and however
  // many levels it has. With levels a quarter of each other, the finest is used from about 180
  // pixels across.
  // A model built to the four-level standard (Model::levelPixels) is switched at the sizes its
  // levels were made for instead; its size is taken as three quarters of its bounding sphere's.
  // [lodscale] Either way the size a level is chosen by is divided by the view's LOD scale (View::lodScale, the
  // camera's: Camera::lodScale), as the original multiplies its measure of detail by it; the least size drawn is not.
  float trianglesPerPixel = 0.55f;
  // The quality setting: scales that wish. 1 is full quality; 0.5 suits a slower card; a very large
  // value forces every model's finest level.
  float detail = 1;
  float minPixels = 1.5f;         // smaller than this on screen: not drawn (a strategic icon's job)
  float shadowMinPixels = 12;     // smaller than this: casts no shadow
  float shadowDetail = 0.25f;     // shadows are drawn from coarser levels: this share of the triangles
  // The scene's solid models that are large on screen first lay their depth alone (with the holes of
  // leaves and of what is being built), and are then shaded writing no depth, so that the full shading
  // runs once a pixel, for what is in front, and not again for every leaf behind it. The same picture.
  // ON (2026-10-15, RTX 4090, 1440p, the best of three runs each): units and scenery on the rebuilt
  // Seton's at middle zoom 2.49 -> 1.84 ms, from far 1.97 -> 1.82; the stock map and a battle the same
  // either way. (A first try kept writing depth in the full shading: the card then tests depth only
  // after shading what cuts holes, and it saved nothing. Small models are left out: drawing them twice
  // cost more than it spared.) Viewer --no-prepass: off.
  bool depthPrepass = true;
  // [mcrec] A pass's draws recorded on several threads at once (gfx.h, Device::BeginParallel): its draws in their order cut
  // into stretches, one a thread, each about the same work, none under a few dozen draws. At most this many threads (0:
  // the jobs pool's workers and the thread drawing, jobs.h; 1: all on the thread drawing, as before). The same picture
  // either way. Viewer --record-threads.
  int recordThreads = 0;
  // The blasts' waves (blasts.h, World::blastWaves), set by the world each frame (null: none): what sways in the wind
  // (trees, the undergrowth) bends away from a blast as its wave passes, and sways back. The constants as the
  // blasts' block reads them (Blasts::Constants, ConstantsSize), bound at register b1 for every pass. What sways is
  // then drawn by shaders of their own with the waves in them (made by PrepareBlasts the first time the world wants
  // them: the same shaders with code added, never run, would not draw quite the same, the compiler reordering what
  // they share; without the waves everything is drawn by the shaders as they were).
  const void* blastConstants = nullptr;
  uint32_t blastConstantsSize = 0;
  bool PrepareBlasts();  // false: they cannot be made (then nothing bends)
  // [plants] Plants that react (plants.h, World::plantsReact), set by the world each frame (null: off): what sways and is
  // low (a bush, a fern, a sapling) is pressed over by the units driving through it and springs back, and is burnt where
  // the ground is; a tree the game has felled stops swaying, and one it has burnt loses most of its needles. What sways and
  // the game's burnt trees are then drawn by shaders of their own with it, the blasts' waves in them too (made by
  // PreparePlants the first time the world wants them), after the rest of a pass, together (SortItems); the sun's maps
  // by lighter ones (bent, not burnt; the game's burnt trees' by one of their own). PlantLife::Constants, and its two
  // textures (read at t8, t9: registers of the models' shaders alone).
  const float* plantsConstants = nullptr;
  gfx::Texture plantsPress, plantsBurnt;
  bool PreparePlants();  // false: they cannot be made (then nothing reacts)
  // [undersnow] The low plants under lying snow (under_snow.h, World::plantsUnderSnow), set by the world each frame: while
  // `depth` is above 0 what sways, is no wreck and whose model is lower than `tallest` (its whole height in the world at its
  // own scale: under a tree) is drawn by variants of its shaders that sink it into the snow, bow it and dust it white
  // (made by PrepareUnderSnow, the blasts' waves and plants that react in them too), in every pass and in its motion, after
  // the rest of a pass, together (SortItems); each plant under the tallest at its own scale takes it, a taller one keeps
  // its look. 0, as at first: drawn by the shaders as they were. Its textures are read at t10, t11 (registers of the
  // models' shaders alone).
  struct UnderSnow {
    float depth = 0;                   // world units the snow lies at the most where all of it lies (GroundCover::snowDepth)
    gfx::Texture shelter;              // the snow ground's shelter (Terrain::SnowShelterPicture), or none: not read
    gfx::Texture groundNormals;        // the ground's normals (Terrain::NormalMap)
    float mapSize[2] = {1, 1};         // the map's width and height
    float wind[2] = {0.915f, 0.403f};  // the way the wind blows (x, z: the snow ground's)
    float tune[2] = {1, 1};            // the trees' crowns' and the drifts' weight (Terrain::snowTune y, w)
    float tallest = 1.5f;              // a plant this tall or taller keeps its look (the snow's shelter's tree, World::SnowProps)
    bool materials = false;            // the ground lays the map's snow materials (Terrain::SnowMaterialsLaid: a mottled margin)
  } underSnow;
  bool PrepareUnderSnow();  // false: they cannot be made (then the plants keep their look)
  // (buildfx) The remastered build look (build_effects.h, World::remasteredBuild), set by the world each frame: what is
  // under construction takes its faction's remastered build shimmer, the models being drawn by the variants of their
  // shaders built with it (made by PrepareBuild the first time the world wants them; without it the shaders as they were).
  bool remasteredBuild = false;
  bool PrepareBuild();  // false: they cannot be made (then the build look stays as it was)
  // [stream] Streams as water (stream_water.h, World::streamWater), set by the world each frame: the levels of a stream's
  // props (ModelLod::stream) are left out of every pass (the scene's, the sun's and the lamps' shadow maps), the streams'
  // own pass drawing them instead. False, as at first: drawn as any model.
  bool divertStream = false;
  // Numbers from the last scene (not shadow) pass.
  int drawnInstances = 0;
  int drawnTriangles = 0;
  int shadowTriangles = 0;        // summed over the shadow passes since the last scene pass
  int posedInstances = 0;         // of the drawn ones: those with bones away from rest
  float prepareMs = 0;            // what Prepare took on the processor, averaged over the last frames
  // Every model added this frame, the listed groups' scenery included, for what goes through them besides the
  // passes (the ray tracing scene, ray_scene.h): f(model, the world matrix's top three rows, its team colour (rgb;
  // w how far it is built), whether it is scenery).
  template <class F>
  void ForEachAdded(F&& f) const {
    for (uint32_t g : groupsListed_)
      if (g < groups_.size())
        for (const Entry& e : groups_[g].entries) f(e.model, e.instance.row0, e.instance.row1, e.instance.row2, e.instance.color, true);
    for (const Entry& e : entries_) f(e.model, e.instance.row0, e.instance.row1, e.instance.row2, e.instance.color, e.scenery);
  }
  // [mcrec] The same list in pieces, for going through it on several threads: AddedPieces() of them in the list's order
  // (each listed group, then the frame's own list in stretches of kPieceModels); ForEachAddedIn(k, f) goes through the k-th.
  size_t AddedPieces() const { return groupsListed_.size() + (entries_.size() + kPieceModels - 1) / kPieceModels; }
  template <class F>
  void ForEachAddedIn(size_t piece, F&& f) const {
    if (piece < groupsListed_.size()) {
      const uint32_t g = groupsListed_[piece];
      if (g < groups_.size())
        for (const Entry& e : groups_[g].entries) f(e.model, e.instance.row0, e.instance.row1, e.instance.row2, e.instance.color, true);
      return;
    }
    const size_t from = (piece - groupsListed_.size()) * kPieceModels, to = std::min(from + kPieceModels, entries_.size());
    for (size_t i = from; i < to; ++i) {
      const Entry& e = entries_[i];
      f(e.model, e.instance.row0, e.instance.row1, e.instance.row2, e.instance.color, e.scenery);
    }
  }

 private:
  struct Instance {
    float row0[4], row1[4], row2[4];  // the top three rows of the world matrix
    float color[4];                   // team colour, w: fraction complete
    float bones[4];                   // x: where its bones' rows begin in the bone buffer, -1: at rest; yzw: a ghost's tint (0: none),
                                      // or y -1 and z, w its treads' scroll
    float wear[4];                    // x: where its marks begin in the bone buffer (-1: none), y how many, z its wear over all, w its
                                      // share of the lying snow (-1: the weather's): Wear, unit_wear.h
  };
  // What a pass tests first, for every model of the list, kept apart from the rest so that going
  // through tens of thousands of them reads little memory.
  enum : uint8_t { kScenery = 1, kPosed = 2, kHasGlass = 4, kHasSolid = 8 };
  struct Cull {
    Vec3 centre;
    float radius;
    float sizeRadius;
    uint8_t flags;
  };
  struct Entry {
    const Model* model;
    Vec3 centre;   // of its bounding sphere, in the world
    float radius;      // of the sphere it is surely inside (a posed model's is wider)
    float sizeRadius;  // of the model at rest: what its size on screen is counted by
    Instance instance;
    Pose pose;
    bool posed = false;
    bool scenery = false;
    int rows = -1;  // where its bones' rows begin in rows_ (Prepare)
    uint8_t flags = 0;  // Cull's
    uint32_t key = 0;   // Add's
    uint32_t mark = 0;  // MarkLast's (hidden outlines)
  };
  Entry MakeEntry(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Vec3* tint, const float* treads = nullptr) const;
  // (rows go to `out`: rowsOut_, or with motion on a copy kept on the processor first)
  void BuildRows(Entry* entry, float* out);
  void Push(const Entry& e) {
    entries_.push_back(e);
    culls_.push_back({e.centre, e.radius, e.sizeRadius, e.flags});
  }
  struct Item {
    const ModelLod* lod;
    Entry* entry;
    float bones;  // Instance::bones.x for this pass
    bool near;    // large on screen (depthPrepass: only these have their depth laid first)
    uint64_t key = 0;  // [mcrec] what SortItems sorts it by (SortKey), worked out where it is found, on the testing threads
    int32_t card = -1;  // [gpuscenery] a list the card picked (its draw in CardScenery::Draws of the pass), not one model
  };
  uint64_t SortKey(const ModelLod* lod, const Model* model, bool near) const;
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_, leaves_, shadow_, glass_, dome_, adding_;
  gfx::Pipeline glassRgb_, domeRgb_;  // the same two writing the colour alone (ModelLod::rgbOnly)
  gfx::Pipeline prepass_, prepassLeaves_;  // (depthPrepass: depth alone, the scene's samples)
  gfx::Pipeline laid_, laidLeaves_;        // (depthPrepass: then the full shading, writing no depth)
  // two of the game's lookup textures for the original look (where the game's files have them):
  // the Cybran's highlight by how the sun's mirror image and the sun itself stand to the surface,
  // and the Seraphim's ramp of how strongly a surface mirrors by the angle it is seen at
  gfx::Texture insectLookup_, seraphimFalloff_;
  bool haveInsectLookup_ = false, haveSeraphimFalloff_ = false;
  // the visuals track's lattice for personal shields, a layer per faction (where it has made one)
  gfx::Texture shieldPatterns_;
  bool shieldPatternOf_[4] = {}, triedShieldPatterns_ = false;
  gfx::Buffer boneBuffer_;
  bool countedScene_ = true;
  std::vector<Entry> entries_;
  std::vector<Cull> culls_;  // one for each of entries_, in the same order
  struct Group {
    std::vector<Entry> entries;
    std::vector<Cull> culls;  // one for each of entries
    // the box round all of their bounding spheres: a pass that does not see it passes the group over
    // whole (what each of them would have been tested for one by one)
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    // [gpuscenery] its place in the world's order (SetGroupOrder); whether it has changed since the card was given it; and
    // those of its props the card does not take (drawn by the processor while the card draws the rest)
    uint32_t order = 0xffffffffu;
    bool cardDirty = true;
    std::vector<Entry> rest;
    std::vector<Cull> restCulls;
  };
  std::vector<Group> groups_;
  std::vector<uint32_t> groupsListed_;
  void Consider(const Cull& c, Entry& e, const View& view, bool shadowPass, bool glass, int part, uint8_t needed, float focal,
                float smallest, float wish, std::vector<Item>* out) const;
  // A pass's models are tested in pieces of up to kPieceModels, spread over the processor's cores;
  // each piece's items are put together in the pieces' order afterwards (the same order as one loop).
  static constexpr size_t kPieceModels = 1024;
  struct Piece {
    const Cull* culls;
    Entry* entries;
    size_t count;
  };
  std::vector<Piece> pieces_;
  std::vector<std::vector<Item>> pieceItems_;
  std::vector<Item> sorting_;     // (the sort's second buffer)
  std::vector<uint32_t> buckets_;  // (and its counts)
  void SortItems();
  void DrawItems(const View& view, gfx::Texture environment, bool shadowPass, bool glass);
  std::vector<Item> items_;
  std::vector<Instance> batch_;
  // [mcrec] DrawItems' draws: each a run of items_ (one level's instances, up to the shader's array), in the order drawn
  // (the depth round's first); recorded on several threads in stretches (recordThreads), each stretch with its own copy
  // of the instances; what DrawItems worked out for all of them, and what they drew
  struct Run {
    uint32_t at, n;
    bool depthOnly;
    int32_t card = -1;  // [gpuscenery] (the card's list: an indirect draw)
  };
  std::vector<Run> runs_;
  std::vector<std::vector<Instance>> chunkBatches_;
  static constexpr int kMostStretches = 16;
  static constexpr double kRunWeight = 64;        // (a draw's own work, as many instances' copies)
  static constexpr double kStretchWeight = 3000;  // (a stretch's least work: about 40 draws)
  struct RunSetup {
    bool shadowPass = false, glass = false, prepass = false, blasts = false, plants = false, snowing = false;
    float snowK[12] = {};
    int cardPass = -1;  // [gpuscenery] (the card's pass the card runs are of)
  };
  struct RunCounts {
    int instances = 0, triangles = 0, shadowTriangles = 0;
  };
  int Stretches() const;
  static bool Plain(const ModelLod* l);
  template <class R>
  void RecordRuns(R& to, const RunSetup& s, size_t from, size_t end, std::vector<Instance>& batch, RunCounts* counts) const;
  // Bones: for every posed model a pass has seen, three rows of four numbers per bone: the matrix
  // that takes a vertex of the model at rest to where the bone has carried it (in the model's own
  // space). They are written straight into the frame's copy of one buffer all passes read (rowsOut_,
  // Prepare's), each model's from its Entry::rows on.
  float* rowsOut_ = nullptr;
  std::vector<uint8_t> posedSeen_;   // Prepare's: for each of culls_, 1 when it is posed and some pass sees it
  std::vector<Entry*> rowsToBuild_;  // Prepare's: those models, in the order of their rows
  // the shells' hits this frame: which entry, and its hits (Prepare lays them in the buffer after the bones)
  struct ShellHits {
    size_t entry;
    std::vector<Vec4> hits;
  };
  std::vector<ShellHits> shellHits_;
  // the units' marks this frame (Wear): which entry, how many rows (three a mark, and its dirt's: unit_wear.h), where
  // its rows begin in wearRows_, and where Prepare laid them in the bone buffer (after the hits; -1: not, no pass sees it)
  struct WearRows {
    size_t entry;
    int rows;
    size_t from;
    int64_t at;
  };
  std::vector<WearRows> wears_;
  gfx::Texture grimeLayers_;  // [grime] the units' shared grime layers (unit_grime.h), made once a unit wears grime
  bool grimeWanted_ = false;
  std::vector<Vec4> wearRows_;
  bool prepared_ = false;

  // motion: each keyed model's place and bones of the frame before (its bones' rows in lastRows_, from
  // `bones` on in fours of floats; -1: at rest); what Prepare saves for the next frame; and the frame's
  struct LastPlace {
    const Model* model = nullptr;
    float row0[4], row1[4], row2[4];
    int bones = -1;
  };
  std::unordered_map<uint32_t, LastPlace> last_, lastNext_;
  std::vector<float> lastRows_, lastRowsNext_, rowsCopy_;

 public:
  // [views] The places and bones motion keeps of the frame before, for one view of a picture with several (views.h): each
  // view's own, swapped in around its frame (the models it lists are not another view's).
  struct Before {
    std::unordered_map<uint32_t, LastPlace> last;
    std::vector<float> rows;
    bool cardOn = false;  // [step2b] (cardByNeed's choice and its count of frames toward the other: each view its own)
    int cardTurn = 0;
  };
  void SwapBefore(Before& b) {
    last_.swap(b.last);
    lastRows_.swap(b.rows);
    std::swap(cardPays_, b.cardOn), std::swap(cardTurn_, b.cardTurn);  // [step2b]
  }

 private:
  // for each of entries_: where its bones of the frame before lie in this frame's bone buffer (-1: at
  // rest), and whether it is known at all (-2: not)
  std::vector<int> lastBonesAt_;
  struct MotionItem {
    const ModelLod* lod;
    const Entry* entry;
    float bones;
  };
  std::vector<MotionItem> motionItems_;  // what the scene's passes drew of the keyed models
  std::vector<MotionItem> swayItems_;    // [leftover] and of what sways without a key (swayMotion)
  gfx::Pipeline swayMotionPipeline_;     // [leftover] (made the first time it is wanted)
  bool swayMotionTried_ = false;
  gfx::Pipeline swaySnowMotionPipeline_;  // [undersnow] (the same for the low plants under the snow: made with PrepareUnderSnow)
  void DrawSwayMotion(const View& view, const Mat4& now, const Mat4& before, float timeBefore);
  std::vector<MotionItem> markedItems_;  // (hidden outlines) what they drew of the marked models
  int marks_ = 0;                        // (hidden outlines) models marked this frame
  bool markedSorted_ = false;
  struct LastInstance {
    float row0[4], row1[4], row2[4];
    float bones[4];  // x: where its bones of the frame before begin (-1: at rest); y: 1 = known
  };
  std::vector<LastInstance> lastBatch_;
  std::vector<LastInstance> lastOf_;  // for each of entries_ the camera may see: its place of the frame before (Prepare's)
  gfx::Pipeline motionPipeline_;
  // the pipelines what sways is drawn with, and the same with the blasts' waves (PrepareBlasts: -1 they cannot be made)
  struct Bending {
    gfx::Pipeline original;
    gfx::PipelineDesc desc;
    gfx::Pipeline bending;
    gfx::Pipeline plants;       // [plants] (PreparePlants)
    gfx::Pipeline plantsBurnt;  // [plants] (the shadows' only: the game's burnt trees)
    gfx::Pipeline snow;         // [undersnow] (PrepareUnderSnow)
  };
  Bending bending_[8];
  std::string blastSource_;
  int blastsMade_ = 0;
  std::string plantsSource_, plantsShadowSource_, plantsShadowBurntSource_;  // [plants]
  int plantsMade_ = 0;        // [plants] (0 not yet, 1 made, -1 they cannot be made)
  std::string snowSource_, snowShadowSource_;  // [undersnow]
  int snowMade_ = 0;                           // [undersnow] (0 not yet, 1 made, -1 they cannot be made)
  // [undersnow] Whether a level is drawn under the snow this frame (underSnow): what sways, no wreck, of a model lower than a
  // tree at its own scale (each instance's own height is tested again by its shader)
  bool Sinks(const ModelLod* lod, const Model* model) const {
    return underSnow.depth > 0 && snowMade_ > 0 && lod->sway > 0 && !lod->wreck &&
           (model->boundsMax.y - model->boundsMin.y) * model->scale < underSnow.tallest;
  }
  void UnderSnowConstants(float* to) const;  // [undersnow] (k0, k1, k2 of under_snow.h: twelve floats)
  // (buildfx) the pipelines that may draw something under construction, and their remastered build look's variants
  struct BuildVariant {
    gfx::Pipeline original;
    gfx::PipelineDesc desc;
    gfx::Pipeline variant;
  };
  std::vector<BuildVariant> buildVariants_;
  std::string buildSource_;
  int buildMade_ = 0;  // (0 not yet, 1 made, -1 they cannot be made)

  // [gpuscenery] The still scenery on the card (cardScenery; card_scenery.h): its record of the groups and its passes, whether
  // it picked this frame's, the views it picked for (the camera's first, then the cascades', in Prepare's order) and the
  // camera's motion of what sways among them (-1: none), an entry for each of its models (what a draw's model is read
  // from), and the frame's number
  std::unique_ptr<CardScenery> card_;
  bool cardNow_ = false;
  std::vector<const View*> cardViews_;
  int cardSway_ = -1;
  std::vector<Entry> cardEntries_;
  uint64_t cardFrame_ = 0;
  bool CardPrepare(const std::vector<const View*>& views);
  int CardPassOf(const View& view) const;
  // [step2b] (cardByNeed: whether the card picks this frame, and the frames in a row that asked for the other; the groups
  // that changed given to the card while it does not pick, so that the frame it picks again has only its own to give)
  bool cardPays_ = false;
  int cardTurn_ = 0, cardSwitches_ = 0;
  bool CardPays();
  void CardKeep();
  void CardGive();
  // its variants of the models' pipelines (each pipeline's id and its variant reading the card's instances), their
  // sources, which variants the last check found wanted (blasts', plants', snow's, build's, sway motion's), and whether they
  // were all made then (false: not all yet, or one could not be)
  std::vector<std::pair<uint32_t, gfx::Pipeline>> cardOf_;
  std::string cardSources_[10];
  gfx::PipelineDesc swayMotionDesc_, swaySnowMotionDesc_;
  int cardWanted_ = -1;
  bool cardReady_ = false, cardFailed_ = false;
  std::string baseSource_;
  bool CardPipelines();
  gfx::Pipeline CardOf(gfx::Pipeline p) const {
    for (const auto& c : cardOf_)
      if (c.first == p.id) return c.second;
    return {};
  }
};

}  // namespace render
