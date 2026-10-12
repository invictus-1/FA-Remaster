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

gfx::Texture TextureCache::SkyCube(const std::string& path) {
  if (path.empty()) return greyCube_;
  const std::string key = "sky:" + moho::Vfs::Normalize(path);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  gfx::Texture result = greyCube_;
  DdsImage image;
  std::string error;
  auto file = vfs_->ReadFile(moho::Vfs::Normalize(path));
  if (!file || !ParseDds(*file, &image, &error) || !image.cube || image.width != image.height) {
    fprintf(stderr, "render: sky cube %s: %s\n", path.c_str(), !file ? "file not found" : error.empty() ? "not a cube map" : error.c_str());
  } else {
    // every face as RGBA, then each level from the one above by averaging 2 x 2 pixels
    std::vector<uint8_t> all, face, next;
    int mips = 0;
    bool ok = true;
    for (int layer = 0; layer < 6 && ok; ++layer) {
      ok = DecodeRgba(image, layer, &face);
      mips = 0;
      for (int size = image.width; ok && size >= 1; size /= 2, ++mips) {
        all.insert(all.end(), face.begin(), face.end());
        if (size == 1) {
          ++mips;
          break;
        }
        const int half = size / 2;
        next.resize(static_cast<size_t>(half) * static_cast<size_t>(half) * 4);
        for (int y = 0; y < half; ++y)
          for (int x = 0; x < half; ++x)
            for (int c = 0; c < 4; ++c) {
              const size_t a = (static_cast<size_t>(2 * y) * static_cast<size_t>(size) + static_cast<size_t>(2 * x)) * 4 + static_cast<size_t>(c);
              const size_t row = static_cast<size_t>(size) * 4;
              next[(static_cast<size_t>(y) * static_cast<size_t>(half) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] =
                  static_cast<uint8_t>((face[a] + face[a + 4] + face[a + row] + face[a + row + 4] + 2) / 4);
            }
        face.swap(next);
      }
    }
    if (ok) {
      gfx::TextureDesc d;
      d.width = d.height = image.width;
      d.mips = mips;
      d.layers = 6;
      d.cube = true;
      d.format = gfx::Format::RGBA8;
      d.data = all.data();
      d.dataSize = all.size();
      gfx::Texture t = device_->CreateTexture(d);
      if (t) result = t;
    } else {
      fprintf(stderr, "render: sky cube %s: its pixel format cannot be decoded\n", path.c_str());
    }
  }
  cache_.emplace(key, result);
  return result;
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
