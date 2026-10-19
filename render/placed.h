// Meshes the game's interface script places in the world (its WorldMesh: a factory's rally point,
// the marks under the cursor, the mark where an order lands). Each names its mesh file, its albedo
// texture and a shader of the original's; what those shaders do is drawn here by the name:
//  - "CommandFeedback", "CommandFeedback2", "RallyPoint": the texture's own colour, unlit, over
//    everything (no depth is tested), larger the farther the camera is so that it can still be
//    seen; over its lifetime it shrinks to 0.7 of its size (CommandFeedback2: grows to 1.1), and
//    the two feedback ones fade out with it;
//  - [pings] "CommandFeedback3", "CommandFeedback4": the texture's own colour, unlit, blended by the texture's alpha
//    (nothing cut), at the mesh's own place and size (no growing from afar, no lifetime): CommandFeedback3 over
//    everything, CommandFeedback4 hidden by what is in front of it, its depth taken a little nearer the camera, as
//    the original's depth bias of -0.0001 takes it. Not only the interface's: the meshes of script entities and units
//    whose mesh blueprint names one of the two (FAF's pings, SimPing.lua; the ferry beacons) are drawn here too
//    (TakeModel), never by the mesh renderer: no light, no shadow, not in the sun's shadow maps, the water's
//    reflections or the ray tracing scene (the original's technique has no depth or reflection stage);
//  - "FakeRings": the texture's colour, unlit, hidden by what is in front; how much of it shows
//    is its fraction-complete number;
//  - anything else ("Unit"): an ordinary model, lit like a unit (World hands those to the mesh
//    renderer; they are not drawn here).
// Both looks draw them alike (the modern look's picture being linear light, the colour is taken into it as the
// marks on the ground are, decals.cpp).
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "view.h"

namespace render {

struct PlacedMesh {
  std::string mesh, texture, shader;  // the script's MeshName, TextureName, ShaderName
  float uniformScale = 1;
  Vec3 position;
  Quat orientation;
  Vec3 scale{1, 1, 1};
  Vec3 color{1, 1, 1};  // as armies' colours are given (screen values); the army colour of a lit one
  bool hidden = false;
  float lifetime = 0;   // seconds its shader's lifetime runs (0: none set), and how far into it it is
  float age = 0;
  float fractionComplete = 1;
  float lodCutoff = 1000;  // [pings] the mesh description's LODCutoff (the original's default 1000; MinimapStamps)
};

class PlacedMeshes {
 public:
  PlacedMeshes(gfx::Device* device, ModelCache* models) : device_(device), models_(models) {}
  bool Init(std::string* error);
  // Is it one of the shaders drawn here (not a lit model)?
  static bool Flat(const std::string& shader);
  // [pings] Which of "CommandFeedback3" and "CommandFeedback4" it is (3, 4; any case), 0 for any other name; and the
  // same for a model by its finest level's shader.
  static int Feedback(const std::string& shader);
  static int Feedback(const Model* model) { return model && model->ok && !model->lods.empty() ? Feedback(model->lods[0].shaderName) : 0; }
  // [pings] A new frame: the models taken for the last are let go (World::Begin).
  void Begin() { taken_.clear(); }
  // [pings] A model of one of those two placed for this frame (`world`: model to world, its scale and size in it): kept
  // to be drawn by Draw, and true; false for any other model, which the caller draws as before.
  bool TakeModel(const Model* model, const Mat4& world);
  bool AnyTaken() const { return !taken_.empty(); }
  // [pings] How much nearer the camera CommandFeedback4 takes its depth: the original's bias of -0.0001 on a depth of 0 at
  // its near plane and 1 at its far one, its near plane a hundredth of the camera's distance (at least 0.01; its far
  // one 17000 beyond: CameraImpl's), is a step of the same size everywhere in a reversed depth of zNear / distance, as
  // this renderer's: zNear * 0.0001 / that near plane.
  static float DepthPull(float zNear, float cameraDistance);
  // Into the scene's pass, after everything solid and the water: the models taken this frame, then `meshes`.
  // `depthPull`: DepthPull's.
  void Draw(const View& view, const std::vector<PlacedMesh>& meshes, float night, float depthPull = 0);
  int drawn = 0;

 private:
  struct Taken {
    const Model* model;
    Mat4 world;
    int kind;
  };
  // (one draw: the model's finest level by those rows of model to world; `params` as the shader's)
  void DrawOne(const gfx::Pipeline& pipeline, const View& view, const ModelLod& lod, const float rows[12], const float params[4],
               gfx::Pipeline* bound);
  gfx::Device* device_;
  ModelCache* models_;
  gfx::Pipeline over_, tested_;
  gfx::Pipeline feedback3_, feedback4_;  // [pings]
  std::vector<Taken> taken_;             // [pings]
};

// [pings] What the original draws of meshes on its minimap for the techniques whose cartographicTechnique is
// "CartographicFeedback" (FAF's mesh.fx, effects.nx2: CommandFeedback, CommandFeedback2, 3 and 4, RallyPoint, FakeRings,
// FakeRingsNoDepth): the minimap is a cartographic view (minimap.lua: SetCartographic(true)), whose
// Cartographic::RenderMeshes (0x7d2cb0) batches every mesh in its view (MeshRenderer::Batch, 0x7dfa00) and draws each by
// its technique's cartographicTechnique (MeshRenderer::RenderCartographic, 0x7dff30). CartographicFeedback: opaque, the
// albedo's colour (its alpha not counted), tested against the depth as it is there and cut below the water's surface
// (CartographicFeedbackPS), at the mesh's own place and size. FAF skips the minimap's meshes altogether while
// cam_DefaultMiniLOD is 0 (its patch at 0x7d1ae9), which its minimap option "Disable meshes" sets, ticked unless the
// player unticked it: the host says which (WorldPicture::SetMinimapMeshes).
// Which meshes reach the minimap: a mesh is drawn while the camera's level of detail at its place is within its level's
// cutoff (Mesh::ComputeLOD 0x7dda50); that measure is the width of the world the view spans across at the place's depth
// times the camera's LOD scale (GeomCamera3::Init 0x4705ed, SetLODScale 0x470b90), the minimap's 1.8 (cam_DefaultMiniLOD
// when it is made, minimap.lua; CUIWorldView 0x86e7ec). A blueprint's mesh fades over 20 beyond its last level's cutoff
// and is left out from 16 beyond it (Mesh::Load 0x7ddbfb sets that level's fade, the fade length 20 and the least
// dissolve 0.2: 0xf57f00, 0xf57f04); the interface's WorldMesh has no fade and its cutoff is its description's
// LODCutoff, 1000 when none (CUIWorldMesh::SetMesh 0x86b2fc). So FAF's pings (99999) always show; the ferry beacons
// (100) and the order marks (1000) only where 1.8 times the width the minimap spans is within that (a map of less than
// about 10 km for the marks).
// The stamps are made on the processor into the minimap's picture, from each mesh's file and albedo read once.
class MinimapStamps {
 public:
  struct View {
    Mat4 matrix;  // the minimap's view and projection: a place in the world to its pixel as WorldPicture::MinimapProject
    std::function<float(float, float)> ground;          // the ground's height at x, z (what the stamps are tested against)
    Vec3 eye, forward;                                  // the minimap camera's place and look
    float widthPerDepth = 0;  // the world its view spans across, at a depth of 1 (2 tan(fovY / 2) x its aspect)
    float lodScale = 1.8f;    // the minimap camera's LOD scale (cam_DefaultMiniLOD when the minimap is made)
    float water = -1e9f;      // the water's surface (very low: no water)
  };
  // Is that technique's cartographicTechnique "CartographicFeedback" (any case)?
  static bool Cartographic(const std::string& shader);
  // Stamps what of `frame` (script entities, units, projectiles; seen) and of `placed` (the interface's meshes, not
  // hidden) is drawn so on the minimap into `pixels` (0xAARRGGBB, `width` x `height`), under what is laid after.
  // Returns how many were stamped.
  int Stamp(ModelCache& models, TextureCache& files, const Frame& frame, const std::vector<PlacedMesh>& placed, const View& view,
            uint32_t* pixels, int width, int height);

 private:
  struct Look {  // a mesh's triangles and its albedo, read once
    bool ok = false;
    std::vector<float> vertices;  // x, y, z, u, v each, in the mesh's own units
    std::vector<uint32_t> indices;
    std::vector<std::vector<uint8_t>> levels;  // the albedo's colour (RGB), halved down to one pixel
    std::vector<int> levelWidth, levelHeight;
  };
  struct Level {
    float cutoff = 0;  // LODCutoff (0: none)
    bool cartographic = false;
    std::string mesh, albedo;
  };
  struct Blueprint {
    bool ok = false;
    float scale = 1;  // UniformScale
    std::vector<Level> levels;
  };
  const Look& LookOf(TextureCache& files, const std::string& mesh, const std::string& albedo);
  const Blueprint& BlueprintOf(TextureCache& files, const std::string& path);
  // (a blueprint's mesh placed by `place` (its scale in it) at `at`: stamped where its level of detail reaches)
  bool StampBlueprint(TextureCache& files, const Blueprint& bp, const Mat4& place, Vec3 at, const View& view, uint32_t* pixels, int width,
                      int height);
  void Fill(const Look& look, const Mat4& place, const View& view, uint32_t* pixels, int width, int height);
  std::unordered_map<std::string, Look> looks_;
  std::unordered_map<std::string, Blueprint> blueprints_;
  const std::vector<std::string>* names_ = nullptr;  // the frame's blueprint names the two below are for
  std::vector<const Blueprint*> byName_;
  std::vector<uint8_t> named_;
  // the highest point stamped at each pixel, where topCall_ says it was in this call (call_: no clearing each call)
  std::vector<float> top_;
  std::vector<uint32_t> topCall_;
  uint32_t call_ = 0;
  std::vector<float> corners_;  // (a stamp's corners in the world and on the minimap: kept from call to call)
  std::vector<uint8_t> cornersOn_;
};

// [pings] The viewer's --ping-test, without a game: what FAF's SimPing.lua makes for each kind of ping (alert, move,
// attack, nuke), a script entity with the ping's mesh 2 above the ground, its size pulsing between 0.2 and 0.6 as the
// script sets it every beat (MATH_Lerp(sin(t), -0.5, 0.5, 0.3, 0.5), t going up 0.3 a beat), in a row round `around`;
// and a UEF ferry beacon (UEB5102, whose mesh is drawn by CommandFeedback4) beside them. `clock` in seconds.
void PingTestFrame(Vec3 around, double clock, const std::function<float(float, float)>& ground, Frame* frame,
                   std::vector<std::string>* names);

}  // namespace render
