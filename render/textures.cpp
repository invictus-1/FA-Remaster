#include "textures.h"

#include <cstdio>

#include "core/vfs.h"
#include "dds.h"

namespace render {
namespace {

gfx::Texture Solid(gfx::Device* device, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const uint8_t pixel[4] = {r, g, b, a};
  gfx::TextureDesc d;
  d.width = d.height = 1;
  d.format = gfx::Format::RGBA8;
  d.data = pixel;
  d.dataSize = 4;
  return device->CreateTexture(d);
}

}  // namespace

TextureCache::TextureCache(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {
  white_ = Solid(device, 255, 255, 255, 255);
  black_ = Solid(device, 0, 0, 0, 0);
  flatNormal_ = Solid(device, 128, 128, 255, 128);
  uint8_t faces[6 * 4];
  for (int i = 0; i < 6; ++i) {
    faces[i * 4] = 110;
    faces[i * 4 + 1] = 130;
    faces[i * 4 + 2] = 150;
    faces[i * 4 + 3] = 255;
  }
  gfx::TextureDesc cube;
  cube.width = cube.height = 1;
  cube.layers = 6;
  cube.cube = true;
  cube.format = gfx::Format::RGBA8;
  cube.data = faces;
  cube.dataSize = sizeof faces;
  greyCube_ = device->CreateTexture(cube);
}

gfx::Texture TextureCache::FromDds(std::string_view file, const char* what) {
  DdsImage image;
  std::string error;
  if (!ParseDds(file, &image, &error)) {
    fprintf(stderr, "render: texture %s: %s\n", what, error.c_str());
    return {};
  }
  gfx::Texture t = device_->CreateTexture(image.Desc());
  if (!t) fprintf(stderr, "render: texture %s: the device refused it\n", what);
  return t;
}

gfx::Texture TextureCache::Get(const std::string& path, gfx::Texture fallback) {
  if (path.empty()) return fallback;
  std::string key = moho::Vfs::Normalize(path);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second ? it->second : fallback;
  gfx::Texture t;
  if (auto file = vfs_->ReadFile(key)) t = FromDds(*file, key.c_str());
  else fprintf(stderr, "render: texture %s: file not found\n", key.c_str());
  cache_.emplace(std::move(key), t);
  return t ? t : fallback;
}

}  // namespace render
