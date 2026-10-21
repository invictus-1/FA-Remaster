// Textures by virtual path, each loaded once from the game's files.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx.h"

namespace moho {
class Vfs;
}

namespace render {

class TextureCache {
 public:
  TextureCache(gfx::Device* device, const moho::Vfs* vfs);
  ~TextureCache();  // [hitches] (stops FetchLater's thread)

  // The texture at a virtual path ("/env/evergreen2/layers/evgrass005_albedo.dds"). A missing or
  // unreadable file is reported once and gives `fallback`.
  gfx::Texture Get(const std::string& path, gfx::Texture fallback = {});
  // A texture from .dds bytes held in memory (the ones inside a map file). Not cached.
  gfx::Texture FromDds(std::string_view file, const char* what);
  // A cube map with a full chain of mip levels made here, each a blurrier copy of the one above
  // (the game's sky cubes have one level only). The modern lighting reads surroundings from it:
  // sharp levels for mirror-like surfaces, the blurriest for light from all sides.
  gfx::Texture SkyCube(const std::string& path);
  // Several .dds files as the layers of ONE texture (a shader reads it as an array): for sets too
  // large for a register each. The files must agree in size, format and number of mip levels (the
  // first one found sets them); a path that is empty, missing or disagrees leaves its layer black
  // and `present[i]` false. Nothing found: no texture. ([seasons] Sizes that differ by whole levels, of
  // one format: the set is read at the smallest, the larger ones from that size down, where their
  // files hold it: a season's material at its 4k tier among 8k ones.)
  gfx::Texture Layers(const std::vector<std::string>& paths, std::vector<bool>* present);

  // The texture detail setting: how many of the largest sizes to leave out of each texture loaded by
  // path (Get, Layers) that is larger than 256 and has the smaller sizes in its file: 0 all (as at
  // first), 1 half the width and height (a quarter of the memory), 2 a quarter. For cards with less
  // memory. Textures already loaded keep theirs: set it before a map is opened. Map data (FromDds),
  // cube maps and arrays of layers given whole stay whole.
  int detail = 0;
  // What the textures made here take on the graphics card, in bytes (all their sizes).
  size_t bytes = 0;

  // Files read ahead (virtual paths): read on the worker threads, all together, and kept until
  // ReadFile takes them; what is here already as a texture, or kept, is left out. For the models
  // seen for the first time in a frame: their files' reading is most of their making.
  void Fetch(const std::vector<std::string>& paths);
  // A file's bytes: those read ahead (taken, and so let go of), else read now. Get reads through it,
  // and the models' meshes do.
  std::optional<std::string> ReadFile(const std::string& path);
  // Lets go of what was read ahead and not taken.
  void DropFetched() { fetched_.clear(); }
  // [hitches] Files read ahead without waiting: on a thread of their own, one after another, for what will be wanted in a
  // while (the next tier of a structure that has appeared, the material of a remastered pad laid somewhere), so that its
  // making later takes no reading. ReadFile takes them (waiting for the one being read, reading itself one still queued);
  // what is here already as a texture, kept or asked for is left out. Arrived: 1 a file's bytes are here (or it is made
  // already), 2 they were here and something has taken them since (a ReadFile of another's: read it again to have it),
  // -1 it cannot be read, 0 still to come (or never asked for).
  void FetchLater(const std::vector<std::string>& paths);
  int Arrived(const std::string& path);
  bool Made(const std::string& path) const;

  gfx::Texture White() const { return white_; }            // (1, 1, 1, 1)
  gfx::Texture Black() const { return black_; }            // (0, 0, 0, 0)
  gfx::Texture FlatNormal() const { return flatNormal_; }  // (0.5, 0.5, 1, 0.5): a normal pointing straight out
  gfx::Texture GreyCube() const { return greyCube_; }      // a cube map of one colour, for a missing sky

 private:
  gfx::Texture Make(std::string_view file, const char* what, int drop);
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  std::unordered_map<std::string, gfx::Texture> cache_;
  std::unordered_map<std::string, std::string> fetched_;  // read ahead, not yet taken
  struct Later;                   // [hitches] (FetchLater's queue and thread: textures.cpp)
  std::unique_ptr<Later> later_;  // (made the first time it is asked for)
  gfx::Texture white_, black_, flatNormal_, greyCube_;
};

}  // namespace render
