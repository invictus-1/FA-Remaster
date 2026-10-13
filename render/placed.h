// Meshes the game's interface script places in the world (its WorldMesh: a factory's rally point,
// the marks under the cursor, the mark where an order lands). Each names its mesh file, its albedo
// texture and a shader of the original's; what those shaders do is drawn here by the name:
//  - "CommandFeedback", "CommandFeedback2", "RallyPoint": the texture's own colour, unlit, over
//    everything (no depth is tested), larger the farther the camera is so that it can still be
//    seen; over its lifetime it shrinks to 0.7 of its size (CommandFeedback2: grows to 1.1), and
//    the two feedback ones fade out with it;
//  - "FakeRings": the texture's colour, unlit, hidden by what is in front; how much of it shows
//    is its fraction-complete number;
//  - anything else ("Unit"): an ordinary model, lit like a unit (World hands those to the mesh
//    renderer; they are not drawn here).
#pragma once
#include <string>
#include <vector>

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
  // Into the scene's pass, after everything solid and the water.
  void Draw(const View& view, const std::vector<PlacedMesh>& meshes, float night);
  int drawn = 0;

 private:
  gfx::Device* device_;
  ModelCache* models_;
  gfx::Pipeline over_, tested_;
};

}  // namespace render
