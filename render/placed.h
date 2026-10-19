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

// [pings] The viewer's --ping-test, without a game: what FAF's SimPing.lua makes for each kind of ping (alert, move,
// attack, nuke), a script entity with the ping's mesh 2 above the ground, its size pulsing between 0.2 and 0.6 as the
// script sets it every beat (MATH_Lerp(sin(t), -0.5, 0.5, 0.3, 0.5), t going up 0.3 a beat), in a row round `around`;
// and a UEF ferry beacon (UEB5102, whose mesh is drawn by CommandFeedback4) beside them. `clock` in seconds.
void PingTestFrame(Vec3 around, double clock, const std::function<float(float, float)>& ground, Frame* frame,
                   std::vector<std::string>* names);

}  // namespace render
