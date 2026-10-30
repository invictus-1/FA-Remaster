// Models: a blueprint's meshes (.scm) and textures, loaded once and kept on the graphics card.
// A model has one or more levels of detail; the blueprint says up to which distance each is used.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "textures.h"

namespace moho {
class Vfs;
}

namespace render {

// A mesh file's vertex, exactly as stored (68 bytes) and as handed to the shaders.
struct ScmVertex {
  float position[3];
  float normal[3];
  float tangent[3];
  float binormal[3];
  float uv0[2];
  float uv1[2];
  uint8_t bones[4];  // only the first is a bone: each vertex follows one (what sways: the rest, ModelLod::treeStep)
};

// A bone of a model's skeleton, standing at rest: where it is in the model's own space (model
// units), composed down the chain of parents the way the simulation composes it.
struct ModelBone {
  std::string name;
  int parent = -1;
  Vec3 position;
  Quat rotation;
};

// A lamp of a remastered unit (its <id>_lights.bp; docs/render-unit-lights.md): a point of light
// that rides on a bone.
struct ModelLight {
  int bone = -1;         // which of Model::bones it moves with (-1: the unit itself)
  Vec3 place;            // in that bone's own frame, model units
  Vec3 color{1, 1, 1};   // linear
  float radius = 1.5f;   // how far it lights, model units
  float strength = 1;
  float blink = 0;       // seconds for one on-and-off; 0: steady
  Vec3 direction;        // a beam: where it points, in the bone's frame
  float cone = 0;        // a beam: its whole opening angle, degrees; 0: it shines to all sides
  float glow = 0.15f;    // a beam: the share that still shines to all sides close to the lamp
  bool hull = false;     // Kind 'hull': the unit's own faint glow, drawn in its army's colour
};

struct ModelLod {
  // its number in the order levels were made (a wreck's copy gets its own): what the model
  // renderer sorts a pass's meshes by, so the order they are drawn in does not hang on where in
  // memory each was made
  uint32_t serial = 0;
  gfx::Buffer vertices, indices;
  uint32_t indexCount = 0;
  uint32_t vertexCount = 0;  // (the ray tracing scene's structures need it: ray_scene.h)
  uint32_t indexBytes = 2;   // each index's size, 2 or 4 bytes (the ray tracing scene reads them: ray_scene.h)
  gfx::Texture albedo, normals, specular;
  gfx::Texture secondary;  // remastered units: ambient occlusion in red
  // [grime] A remastered unit's wear art (unit_grime.h; loaded while ModelCache::wearArt is set): `wearMasks`, its secondary
  // is the masks texture (<ID>_lab_masks.dds: R the same crease shading, G where grime gathers, B the order damage shows, A
  // where weather dirt sits); its damage set (<ID>_lab_damage_albedo.dds, colour with heat in alpha; _damage_normal.dds).
  bool wearMasks = false;
  gfx::Texture damageAlbedo, damageNormal;
  // Remastered units ("LabPBR"): the specular texture holds the material itself - red: metal,
  // green: roughness, blue: glow, alpha: team colour mask. Otherwise it is the original's spec-team
  // texture (red: reflection, green: highlight, blue: glow, alpha: team colour mask), which the
  // modern lighting has to translate.
  bool labPbr = false;
  // its texture's treads scroll (the mesh blueprint's Scrolling; on a remastered mesh, whose texture is laid out its own
  // way, only where its folder says so: <file>_treads.bp, Treads { Scrolling = true })
  bool scrolling = false;
  // [treads] world units a groove of its treads is deep, where its albedo's alpha holds their height on the strips that
  // scroll (a remastered mesh's Treads { Height = true }; ModelCache::treadDepth; 0: none): the eye looks into the grooves
  float treadDepth = 0;
  bool plainMaterial = false;  // a remastered level without that texture: rough, no metal, no glow
  float cutoff = 0;        // the blueprint's LODCutoff: the distance the ORIGINAL game uses this level up to.
                           // This renderer chooses levels by size on screen instead (mesh_renderer.h).
  std::string shaderName;  // "Unit", "Aeon", "Insect", "Seraphim", "UndulatingNormalMappedAlpha", ...
  // What the shader name means for drawing (from the original's techniques of those names):
  float alphaCut = 0;          // pixels whose albedo alpha is not above this are left out (foliage)
  bool blend = false;          // drawn blended over what is behind, by the albedo's alpha
  bool border = false;         // "MapImager": the machinery a map lies in: lit by the sun's angle alone, its
                               // albedo's alpha added to its colour, glowing where the specular's blue says
  float sway = 0;              // how much it sways in the wind (the original's "Undulating..." trees, the
                               // remaster's foliage): 0 not at all, 1 as a tree does
  float swayHeight = 1;        // the model's height in its own units: the sway grows from the foot to there
  // A group of trees in one mesh (the game's tree groups, the visuals track's; one bone a tree; models.cpp, TagTrees):
  // each vertex of what sways says where its tree stands (its bone indices' second and third bytes: x and z in steps
  // of this many model units, 128 the model's origin) and its tree's own beat (the fourth: 1..255, a share of a whole
  // swing; 0 the model's beat), so that each tree sways on its own (mesh_renderer.cpp). 0: no trees of their own (a
  // single tree, a group's far card: every vertex on one bone, the model's beat as before).
  float treeStep = 0;
  bool leaves = false;         // "LabPBRAlpha": a remastered surface with holes: seen from both sides (lit as
                               // facing whoever looks), its holes' edges smoothed, and its specular texture's
                               // alpha is how much light comes through from the far side (no team colour)
  bool vertexNormals = false;  // lit by the mesh's own normals, no normal map
  // The original draws it into the colour alone, not the frame's alpha (its technique's AlphaState is
  // ..._Write_RGB: GlassAlpha, UEFBuildCube, Cloud, OuterCloud, Explosion, Effect, NukeUEF, ShieldSeraphim):
  // so it adds nothing to the original look's bloom, which reads that alpha as glow.
  bool rgbOnly = false;
  bool noShadow = false;       // "...NoShadow" (the objective arrow's NormalMappedAlphaNoShadow): casts none
  bool glass = false;          // see-through, drawn after the water without writing depth (frames of light, shields)
  bool shield = false;         // "Shield...": a shield's bubble: a faint tinted shell, brighter toward its rim
  Vec3 shieldTint{0.25f, 0.5f, 1.0f};
  bool hidden = false;         // "ShieldFill": the original draws it into the depth alone; here it is not drawn
  bool chrome = false;         // "AeonBuildPuddle": the pool of liquid metal an Aeon building rises from
  bool wreck = false;          // wreckage (ModelCache::Wreck): burnt and dull, no team colour, no glow
  bool fades = false;          // "AlphaFade": a see-through slab that fades with its age (the UEF's build slices)
  // A nuclear explosion's ball of fire, drawn by its age (the instance's colour says it, in seconds):
  // 1 "NukeUEF": churning fire with holes that eats itself away after 20 seconds and hides what burns
  // inside it; 2 "NukeEMP", "NukeQuantum": a shell whose texture slides along it and fades late
  // (nukeShell: how often the texture repeats, how fast it slides, from what age it fades).
  int nuke = 0;
  Vec3 nukeShell{1, 0, 0};
  // Its albedo is the visuals track's (an effect's mesh in the remastered look, effect_files.h): the
  // remastered look's own way of drawing it where it has one (the UEF's ball of fire: section 25).
  bool remasteredEffect = false;
  // Three more of the game's looks that are not lit by the scene: 1 "Flat" (the first game's
  // "TMeshNoLighting": some missiles): its texture's own colours, cut by its alpha; 2
  // "UEFQuantumGate": a see-through field, its texture sliding with a finer copy taken away from
  // it; 3 "PhalanxEffect": a faint see-through ball (the Cybran's missile zapper), two textures
  // sliding over it.
  // "ShieldImpact" (1) and "CybranShieldImpact" (2): the patch that flashes on a shield's bubble
  // where it is hit, its textures sliding over it by its age
  int impact = 0;
  int unlit = 0;
  // A personal shield's shell round a unit (ModelCache::ShieldShell): nothing but round its hits, where its
  // lattice (the albedo) lights up.
  bool shell = false;
  bool frame = false;          // "UEFBuildCube": a box of light round what the UEF builds: see-through, blue,
                               // drifting, not lit (its texture is clouds, with the holes in its alpha)
  bool teamMask = true;        // the team colour replaces the albedo where the spec-team texture says;
                               // otherwise the colour multiplies the albedo
  // Whose look the textures were painted for (the channels mean different things, mesh_renderer.cpp):
  // 0 the UEF's "Unit" and everything else, 1 Aeon, 2 Cybran ("Insect"), 3 Seraphim.
  int faction = 0;
  // [stream] A stream's water (1) or its white water (2) (stream_water.h, World::streamWater): drawn by the streams' own
  // pass while that is on (MeshRenderer::divertStream), and as before while it is off; `streamMesh` its mesh file, which
  // that pass reads for the ribbon's shape.
  int stream = 0;
  std::string streamMesh;
};

struct HitShape;  // (unit_wear.h)
struct MachinerySpec;  // ([machinery] machinery.h)
struct ScmAhead;  // [spikes] (models.cpp: a mesh made ahead)

struct Model {
  bool ok = false;
  bool scrolls = false;  // one of its levels' treads scroll (ModelLod::scrolling)
  std::vector<ModelLod> lods;  // finest first
  float scale = 1;             // model units to world units (the blueprint's Display.UniformScale)
  Vec3 boundsMin, boundsMax;   // of the finest level, in model units
  // [hull sit] A unit's: the height of its top at the rim of its plan, over its origin (model units, finest level): a
  // ship's deck at its sides, as the original's water sheet left it above the water (models.cpp, DeckHeight). Not known:
  // 1e30.
  float deckHeight = 1e30f;
  // The skeleton of the mesh the blueprint names: the one the simulation poses. The vertices of
  // every level follow these bones by index (a level whose file lists its bones in another order,
  // or a remastered mesh tried out in the blueprint's place, is matched to them by name at loading).
  std::vector<ModelBone> bones;
  // [machinery] A remastered factory's machinery (machinery.h): its parts' bones are appended to `bones` after the
  // simulation's (its finest level's own bones past the skeleton, by name), and how they move. Null: none.
  std::shared_ptr<const MachinerySpec> machinery;
  // The box round each bone's part of the finest level at rest (model units; by the skeleton's numbers; lo above hi:
  // no part): where on the model a hit lies, whichever way its bones are posed (unit_wear.h).
  struct BoneBox {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  };
  std::vector<BoneBox> boneBoxes;
  // Its triangles at rest, for where a hit lies on it (unit_wear.h; made while ModelCache::hitShapes is set); null: none.
  std::shared_ptr<const HitShape> hitShape;
  // (debris, debris.h) A unit's: a few places its finest level's paint lies on its texture, spread over its surface by
  // area: each the middle of one of its triangles in its texture (x, y) and how much of the texture a model unit of
  // its surface takes there (z). The debris's pieces wear patches of its texture from these; empty: none known.
  std::vector<Vec3> paintSpots;
  // (debris) Its finest level's specular texture's path ("" none): the debris reads where its paint glows from it (its
  // blue), to keep its pieces' patches off its lights.
  std::string paintMaterial;
  // For a model built to the four-level standard (a remastered unit's export for this engine): the
  // size on screen, in pixels, down to which each level but the last is used - the sizes its levels
  // were made and judged for. Empty: levels are chosen by their triangle counts (mesh_renderer.h).
  std::vector<float> levelPixels;
  std::vector<ModelLight> lights;  // a remastered unit's lamps (at most 16)
  // [hdprints] A remastered unit's own ground prints for the deep tracks (its folder's <ID>_prints.bp, Prints { Tread = {
  // Texture, Width, Length }, Tyre = { Texture, Width, Length }, Foot = { Texture, Size = { x, z } } }; deep_tracks.h): each a
  // picture of how deep the ground is pressed (one channel, white deepest) and its size in world units (a tread's or tyre's
  // picture one piece of the track, laid end to end; a foot's the sole). An empty texture: none of that kind.
  struct Print {
    std::string texture;     // the picture's path
    float width = 0, length = 0;  // world units across (the unit's x) and along (its z; the picture's top its front)
  };
  struct Prints {
    Print tread, tyre, foot;
    bool Any() const { return !tread.texture.empty() || !tyre.texture.empty() || !foot.texture.empty(); }
  } prints;
  // [plants] One of the game's trees (its blueprint's script class Tree or TreeGroup): the simulation knocks it over, burns
  // and removes it, so plants that react never bend or burn it by the picture alone (World::plantsReact).
  bool simTree = false;
  // A personal shield's shell (ModelCache::ShieldShell): its shape, in model units, for OnShell.
  struct ShellShape {
    int along = 2, acrossA = 0, acrossB = 1;  // its long axis and the two across it (0 x, 1 y, 2 z)
    float centre[3] = {0, 0, 0};
    float a = 1, b = 1, half = 1, cap = 1;  // half widths across, half length, each rounded end's length
  } shellShape;
};

// Where a point lies on a shell (ModelCache::ShieldShell) seen from its long axis: a hit on a personal
// shield is told where the shot met the game's shield (a sphere of the blueprint's ShieldSize, often far
// out), and is lit on the shell where that point's way from the axis crosses it. `rest`: a point in the
// unit's own frame at rest, world units; the answer the same.
Vec3 OnShell(const Model& shell, Vec3 rest);

// A mod's own look for a unit's personal shield: its blueprint's Display.PersonalShield (the visuals track's):
//   Display.PersonalShield = { HitOnly = true, Shell = "Capsule", Lattice = "/mods/<mod>/<texture>.dds" }
// HitOnly: nothing shows at rest, only round hits. Shell "Capsule": a smooth capsule round the unit, a little
// off it, instead of the unit's own outline ("Outline", as without it). Lattice: the texture lit round hits:
// tiling, its colour in RGB and how much shows in A (the lines 1, the cells faint); 3.84 world units a tile.
// The capsule's size, in model units, where the model's box would make it too big (fins, masts):
// ShellSize = { half its width (x), half its height (y), its whole length (z) }, and ShellCentre = { x, y, z }
// (else the box's middle); without ShellSize it is made round the model's box.
struct PersonalShieldLook {
  bool any = false;  // the blueprint has one
  bool hitOnly = false;
  bool capsule = false;
  gfx::Texture lattice;
  bool sized = false, centred = false;
  float size[3] = {0, 0, 0};
  float centre[3] = {0, 0, 0};
};

class ModelCache {
 public:
  ModelCache(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures);  // ([spikes] in models.cpp: meshesAhead_)
  ~ModelCache();  // [spikes] (stops the meshes' thread of MakeAhead)

  // The model of a unit blueprint id ("uel0201"). Never null: a unit whose files cannot be read gets
  // a model with ok == false (reported once).
  // `original`: the game's own model even when a remastered one is there (to show both side by side).
  const Model* Unit(const std::string& blueprintId, bool original = false);
  // Where that unit's blueprint file is: the game's own in /units, a mod's in its folder under /mods.
  const std::string& UnitBlueprintPath(const std::string& blueprintId);
  // Reads ahead, on the worker threads, the files of these units' models that are not made yet (their
  // meshes and textures, the remastered ones too), so that making them after takes no reading.
  void Prefetch(const std::vector<std::string>& unitIds);
  // The same for blueprint files (a map's props: "/env/evergreen/props/rocks/rock01_prop.bp").
  void PrefetchBlueprints(const std::vector<std::string>& blueprintPaths);
  // [hitches] The next tier made ahead: a unit kind that has just appeared (`unitId`) whose blueprint names what it
  // upgrades to (General.UpgradesTo: a T1 factory's T2) has that model made before the upgrade asks for it, without a
  // frame waiting for it: the blueprint and then the model's files read on the textures' own thread (FetchLater), its
  // textures made one a frame, then the model itself (AheadStep, once a frame). Made so, the model is the one the upgrade
  // would have made (the same files and steps), only earlier.
  void ReadAheadUpgrade(const std::string& unitId);
  // [spikes] Unit kinds made ahead before any of them is first drawn (what an army may build: World::MakeAhead, from the
  // simulation's own test, SimFrames::WatchBuildable): each kind's textures, listed as Prefetch lists them, made ahead off
  // the frames' thread (TextureCache::MakeAhead), and its meshes read and made on a thread of the cache's own, their
  // buffers prepared (MeshJob: what Build's own steps would make of them); once all are there its model is made
  // (AheadStep): what is left of that is its buffers finished and its textures taken. Then a remastered kind's own wreck
  // (RemasteredWreck) the same way. A kind made or asked about already is left out. (ReadAheadUpgrade's next tiers go the
  // same way since.)
  void MakeAhead(const std::vector<std::string>& unitIds);
  // One step of these (at most one model made); true when it made something. (World::PrepareAhead calls it again while
  // it makes something and a millisecond of the frame is not used up.)
  bool AheadStep();
  size_t AheadLeft() const { return ahead_.size(); }  // [spikes] (kinds asked for and not made yet)
  int madeAhead = 0;  // [hitches] (models made ahead so far)
  int madeAlike = 0;  // [spikes] (models from the simulation's looks taken from the same files' model: FromDisplay)
  bool readAhead = false;  // [hitches] (set by the world each frame: World::readAhead; each unit model made asks ReadAheadUpgrade)
  // Is there a remastered model or material for this unit (a --preview folder, the lab's LabVisuals mod)?
  bool Remastered(const std::string& blueprintId) const;
  // The model of any blueprint file with a Display.Mesh ("/env/evergreen/props/rocks/rock01_prop.bp"),
  // or of a mesh blueprint file itself ("/meshes/game/map-border_squ_uef_mesh.bp").
  // `faction` ("uef", "aeon", "cybran", "seraphim"): in the remastered look an effect's textures are read
  // from the visuals track's "<the texture's folder>/<faction>/" first (a shield's flash, whose mesh three
  // factions share), then as without it.
  // `effect`: the mesh of an effect entity or of a script entity (never a unit's or scenery's): in the
  // remastered look the visuals track's texture of the same path is taken wherever the texture is (the
  // UEF's build cube names /meshes/generic/cube02_albedo.dds), not only under /effects/.
  const Model* Blueprint(const std::string& blueprintPath, bool original = false, const std::string& faction = std::string(),
                         bool effect = false);
  // A model from a mesh file and an albedo texture named outright, with a shader's name (what the
  // game's interface script places in the world, placed.h). One level; `uniformScale` is its scale.
  const Model* Mesh(const std::string& meshPath, const std::string& albedoPath, const std::string& shader, float uniformScale = 1);
  // The same model as wreckage (the game's "..._mesh_wreck": the mesh with a burnt look). `ownScale`: keep
  // the model's scale (a burnt tree); otherwise it is 1 and whoever places it gives the size (a dead
  // unit's wreck: the script sizes the prop).
  const Model* Wreck(const Model* of, bool ownScale);
  // A remastered unit's own wreck (the visuals track's <id>_wreck_lod*.scm and <id>_wreck_lab_*.dds beside the
  // unit's files): drawn as painted - its own colours, no team colour, no glow, no lamps, not swaying - at the scale
  // whoever places it gives (a dead unit's wreck: its prop's). Null where the unit has none (then Wreck's burnt look).
  const Model* RemasteredWreck(const std::string& blueprintId);
  // A unit's own look for its personal shield (from its blueprint file, read once; see PersonalShieldLook).
  const PersonalShieldLook& PersonalShield(const std::string& blueprintId);
  // [wakes] Whose a unit is, for the faction's own effects in the remastered look (effect_files.h): by its id where
  // that says it (UnitFaction: the game's own units, as before), else as its blueprint file names it -
  // General.FactionName "UEF", "Aeon", "Cybran" or "Seraphim", else one of those among its Categories - so that a mod's
  // unit with an id of its own, built like the game's (a ship whose blueprint says FactionName "UEF"), plays its
  // faction's remastered effects (its wakes) whoever else its army has. Read once a kind; "" not known (then the army's,
  // Effects::FactionOf).
  const std::string& Faction(const std::string& blueprintId);
  // The capsule a personal shield lies on round a model (made once a model): along its longest side, its
  // section a superellipse (|x/a|^3.2 + |y/b|^3.2 = 1), rounded at both ends, a fifth of its smaller half
  // width off it all round; in the model's own units, so it is placed as the model is. Its one level is a
  // shell (ModelLod::shell) with `lattice` as its albedo.
  const Model* ShieldShell(const Model* of, const PersonalShieldLook& look);
  // Reads the mesh and texture names of blueprints as the simulation has them after the game's
  // scripts and the mods changed them: the log the engine's tools/dump_display.lua writes. Models
  // listed there take their names from it instead of from the .bp file. Returns the levels read.
  int LoadDisplayTable(const std::string& hostPath);
  // The same from the simulation itself: how one blueprint looks (frame.h, BlueprintLook). Models
  // asked for afterwards take their names from it.
  void SetLook(const BlueprintLook& look);
  // Units' models loaded while this is set keep a shape of their triangles for battle scars (Model::hitShape,
  // unit_wear.h; World sets it with unitScars).
  bool hitShapes = false;
  // [grime] Units' models loaded while this is set take their wear art (ModelLod::wearMasks, damageAlbedo, damageNormal;
  // World sets it with unitGrime or unitDamage): from the unit's textures beside its _lab_ ones, or, for a unit named in
  // `wearArtTrial` (its id, lower case), from the trial folder mounted at /wear-art/<id> (masks/<ID>_ao_masks.dds,
  // damage/<ID>_damage_albedo.dds and _normal.dds: the visuals session's trial layout; the viewer's --wear-art).
  bool wearArt = false;
  std::vector<std::string> wearArtTrial;
  // [treads] Remastered units' models loaded while this is above 0 look into their treads' grooves where their folder
  // gives the treads' height (ModelLod::treadDepth: this many world units deep; the viewer's --tread-depth).
  float treadDepth = 0;
  // [unit lamps] Every remastered unit's lamps this many times the strength its list gives each (1; the viewer's
  // --lamp-scale, set before models are made: to try a factor before a unit's list is changed, docs/render-unit-lights.md).
  float lampScale = 1;

 private:
  struct LodSource {
    float cutoff = 1e9f;
    bool scrolling = false;  // Scrolling
    std::string shader = "Unit";
    std::string mesh, albedo, normals, specular, secondary;  // virtual paths; empty: none
    bool albedoNamed = false, normalsNamed = false, specularNamed = false, secondaryNamed = false;
  };
  struct DisplayEntry {
    float scale = 1;
    std::vector<LodSource> lods;
  };
  // [spikes] (Build's own choice of files, made the same way on the meshes' thread for a kind made ahead: PlanBuild) the
  // mesh whose bones are the skeleton, the remastered model's folder and the name of its files there (a wreck's is
  // "<id>_wreck"), whether it has one, and the levels' sources with its meshes in place
  struct BuildPlan {
    std::string skeletonMesh, own, preview, file;
    bool previewed = false;
    std::vector<LodSource> sources;
  };
  BuildPlan PlanBuild(const std::string& base, std::vector<LodSource> sources, bool original, const std::string& stem) const;
  // [spikes] A kind's meshes made ahead on a thread of the cache's own (MakeAhead): what Build's LoadScm calls would make of
  // its levels' files with the very inputs Build will give them (its plan, its skeleton, each level's flags), their buffers
  // prepared on the device; Build takes each by those inputs (a key of them all) and finishes its buffers, and makes any it
  // does not find as before. `what`, `base`, `stem`: as Build's. The levels' sources are `sources`, or where `blueprint` is
  // named those ReadSources reads from it (a remastered wreck's: with its first mesh `first`, as RemasteredWreck has them).
  // A wreck's textures named by its blueprint are listed for the frames' thread to make ahead (MeshesTextures).
  struct MeshJob {
    std::string what, base, stem;
    std::vector<LodSource> sources;
    std::string blueprint, first;
    bool hitShapes = false;
  };
  struct MeshesAhead;                      // (models.cpp: the thread, its jobs, what it made)
  std::unique_ptr<MeshesAhead> meshesAhead_;
  void AskMeshes(MeshJob job);             // (these on the frames' thread)
  bool MeshesPending(const std::string& what) const;
  std::unique_ptr<ScmAhead> TakeScm(const std::string& key);
  bool TakeBones(const std::string& skeletonMesh, std::vector<ModelBone>* out);
  std::vector<std::string> MeshesTextures(const std::string& what);  // (taken: asked once)
  void DropMeshes(const std::string& what);  // (what was made for it and not taken let go of; a job not begun dropped)
  void MeshesRun();                          // (the meshes' thread)
  void MeshesWork(const MeshJob& job);
  const Model* FromDisplay(const std::string& key, const std::string& base, const DisplayEntry& entry, const std::string& faction,
                           bool effect = false);
  // A blueprint file's levels: their file names, and the model's scale. False when it has none to draw.
  // (`meshBlueprint`: where the blueprint holds no mesh, the mesh blueprint it names instead, if any)
  bool ReadSources(const std::string& path, std::vector<LodSource>* sources, float* scale, std::string* error,
                   std::string* meshBlueprint = nullptr);
  // `stem`: a remastered model's files under another name in its folder (a wreck's: "<id>_wreck"); then no lamps.
  void Build(Model* m, const std::string& what, const std::string& base, std::vector<LodSource> sources, bool original, bool merged,
             const std::string& faction = std::string(), bool effect = false, const std::string& stem = std::string());
  std::unordered_map<std::string, DisplayEntry> display_;  // by unit id, or by a prop's blueprint path
  // [hitches] A unit's model's files (its levels' meshes and textures, the remastered ones too: Prefetch's list); whether
  // that model is made already
  std::vector<std::string> UnitFiles(const std::string& unitId);
  bool UnitMade(const std::string& unitId);
  struct Ahead {           // [hitches] (ReadAheadUpgrade; [spikes] MakeAhead)
    std::string of, path;  // the unit kind that appeared, its blueprint file (empty: asked for itself, MakeAhead)
    std::string to;        // what it upgrades to (once its blueprint is read), or the kind asked for
    std::vector<std::string> files;
    // 0 its blueprint being read, 1 the kind's textures and meshes being made (off the frames' thread), 3 the model,
    // 4 a remastered kind's own wreck (its files asked for with the kind's)
    int stage = 0;
    bool wreck = false;  // [spikes] (it has a remastered wreck of its own: made after the model)
    bool wreckAsked = false;              // [spikes] (its wreck's textures named by its blueprint asked for: wreckFiles)
    std::vector<std::string> wreckFiles;
    std::string meshesOf;                 // [spikes] (its meshes' job's `what`: MeshJob)
  };
  void AskFiles(Ahead* a);  // [spikes] (a kind's files: its textures made ahead, its meshes too (MeshJob); stage 1)
  void FinishAhead(size_t k);  // [spikes] (ahead_[k] done with: erased, what was made for it let go of)
  // [spikes] (FromDisplay) the levels and scale a model from a blueprint file was made from, by its path
  struct BuiltFrom {
    std::vector<LodSource> sources;
    float scale = 1;
  };
  std::unordered_map<std::string, BuiltFrom> builtFrom_;
  static bool SameSources(const std::vector<LodSource>& a, const std::vector<LodSource>& b);
  std::vector<Ahead> ahead_;
  std::vector<std::string> aheadSeen_;  // (the kinds asked about, each once)
  bool makingAhead_ = false;
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  std::unordered_map<std::string, std::unique_ptr<Model>> models_;
  std::unordered_map<std::string, PersonalShieldLook> personalShields_;  // by unit id
  std::unordered_map<std::string, std::string> factions_;  // [wakes] by unit id (Faction)
  std::unordered_map<const Model*, std::unique_ptr<Model>> shells_;      // by the model they lie round
  std::unordered_map<std::string, std::string> unitPaths_;  // unit id -> its blueprint file
  // every mod's unit blueprint file by its name ("<id>_unit.bp"), the first in path order: one search of
  // /mods when the first unit that is not the game's own is asked for (instead of one search each)
  std::unordered_map<std::string, std::string> modUnits_;
  bool modUnitsListed_ = false;
  std::unordered_map<const Model*, std::unique_ptr<Model>> wrecks_[2];  // by the model it is of: [1] with its own scale
};

}  // namespace render
