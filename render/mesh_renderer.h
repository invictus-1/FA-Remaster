// Draws models: every frame the caller lists what is in the world (a model, where, in which colour);
// each pass then draws what of that list it can see, sorted by mesh, each mesh's instances together.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "models.h"
#include "textures.h"
#include "view.h"

namespace render {

class MeshRenderer {
 public:
  MeshRenderer(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  bool Init(std::string* error);

  // Empties the list.
  void Begin();
  // `world` places the model's own units in the world (so it includes the model's scale).
  // `teamColor` is rgb in 0..1; `built` is the fraction complete (1 for a finished unit).
  void Add(const Model* model, const Mat4& world, Vec3 teamColor, float built = 1);
  // Draws the list as `view` sees it: into the scene, or (`shadowPass`) into a shadow map, where
  // only depth is written. `environment` is the cube map the original look reflects in shiny parts.
  void Draw(const View& view, gfx::Texture environment, bool shadowPass = false);

  // Distances are multiplied by this before they are compared with a model's cutoffs: below 1 keeps
  // detail (and the model itself) for longer.
  float lodScale = 1;
  // Numbers from the last scene (not shadow) pass.
  int drawnInstances = 0;
  int drawnTriangles = 0;

 private:
  struct Instance {
    float row0[4], row1[4], row2[4];  // the top three rows of the world matrix
    float color[4];                   // team colour, w: fraction complete
  };
  struct Entry {
    const Model* model;
    Vec3 centre;   // of its bounding sphere, in the world
    float radius;
    Instance instance;
  };
  struct Item {
    const ModelLod* lod;
    const Instance* instance;
  };
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_, shadow_;
  std::vector<Entry> entries_;
  std::vector<Item> items_;
  std::vector<Instance> batch_;
};

}  // namespace render
