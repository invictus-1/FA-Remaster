// Draws models: every frame the caller adds what is to be seen (a model, where, in which colour), then
// Draw sorts that by mesh and draws each mesh's instances together.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "models.h"
#include "scmap.h"
#include "textures.h"
#include "view.h"

namespace render {

class MeshRenderer {
 public:
  MeshRenderer(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  bool Init(std::string* error);

  // Start of a frame's list; `view` decides what is in sight and which level of detail is used.
  void Begin(const View& view);
  // `world` places the model's own units in the world (so it includes the model's scale).
  // `teamColor` is rgb in 0..1; `built` is the fraction complete (1 for a finished unit).
  // Returns false when the model is not drawn: out of sight, or beyond its last level of detail.
  bool Add(const Model* model, const Mat4& world, Vec3 teamColor, float built = 1);
  // Draws and forgets everything added. `environment` is the cube map reflected by shiny parts.
  void Draw(const MapLighting& lighting, gfx::Texture environment);

  // Distances are multiplied by this before they are compared with a model's cutoffs: below 1 keeps
  // detail (and the model itself) for longer.
  float lodScale = 1;
  int drawnInstances = 0;
  int drawnTriangles = 0;

 private:
  struct Instance {
    float row0[4], row1[4], row2[4];  // the top three rows of the world matrix
    float color[4];                   // team colour, w: fraction complete
  };
  struct Item {
    const ModelLod* lod;
    Instance instance;
  };
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_;
  View view_;
  std::vector<Item> items_;
  std::vector<Instance> batch_;
};

}  // namespace render
