// Textures by virtual path, each loaded once from the game's files.
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

#include "gfx.h"

namespace moho {
class Vfs;
}

namespace render {

class TextureCache {
 public:
  TextureCache(gfx::Device* device, const moho::Vfs* vfs);

  // The texture at a virtual path ("/env/evergreen2/layers/evgrass005_albedo.dds"). A missing or
  // unreadable file is reported once and gives `fallback`.
  gfx::Texture Get(const std::string& path, gfx::Texture fallback = {});
  // A texture from .dds bytes held in memory (the ones inside a map file). Not cached.
  gfx::Texture FromDds(std::string_view file, const char* what);
  // A cube map with a full chain of mip levels made here, each a blurrier copy of the one above
  // (the game's sky cubes have one level only). The modern lighting reads surroundings from it:
  // sharp levels for mirror-like surfaces, the blurriest for light from all sides.
  gfx::Texture SkyCube(const std::string& path);

  gfx::Texture White() const { return white_; }            // (1, 1, 1, 1)
  gfx::Texture Black() const { return black_; }            // (0, 0, 0, 0)
  gfx::Texture FlatNormal() const { return flatNormal_; }  // (0.5, 0.5, 1, 0.5): a normal pointing straight out
  gfx::Texture GreyCube() const { return greyCube_; }      // a cube map of one colour, for a missing sky

 private:
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  std::unordered_map<std::string, gfx::Texture> cache_;
  gfx::Texture white_, black_, flatNormal_, greyCube_;
};

}  // namespace render
