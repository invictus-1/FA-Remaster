#include "textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/vfs.h"
#include "dds.h"
#include "linalg.h"

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

gfx::Texture TextureCache::Layers(const std::vector<std::string>& paths, std::vector<bool>* present) {
  present->assign(paths.size(), false);
  DdsImage first;
  std::vector<uint8_t> all;
  size_t each = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (paths[i].empty()) continue;
    auto file = vfs_->ReadFile(paths[i]);
    DdsImage image;
    std::string error;
    if (!file || !ParseDds(*file, &image, &error) || image.cube) continue;
    if (!each) {
      first = image;
      each = image.dataSize;
      all.assign(each * paths.size(), 0);
    } else if (image.width != first.width || image.height != first.height || image.format != first.format || image.mips != first.mips ||
               image.dataSize != each) {
      fprintf(stderr, "render: %s: not of the size and kind of the others of its set; left out\n", paths[i].c_str());
      continue;
    }
    std::memcpy(all.data() + each * i, image.data, each);
    (*present)[i] = true;
  }
  if (!each) return {};
  gfx::TextureDesc td;
  td.width = first.width;
  td.height = first.height;
  td.mips = first.mips;
  td.layers = static_cast<int>(paths.size());
  td.format = first.format;
  td.data = all.data();
  td.dataSize = all.size();
  return device_->CreateTexture(td);
}

namespace {

// The direction a pixel of a cube map's face looks in (the faces in the order +x, -x, +y, -y, +z,
// -z; u to the right and v down across the face, each -1..1), not of unit length.
Vec3 CubeDirection(int face, float u, float v) {
  switch (face) {
    case 0: return {1, -v, -u};
    case 1: return {-1, -v, u};
    case 2: return {u, 1, v};
    case 3: return {u, -1, -v};
    case 4: return {u, -v, 1};
    default: return {-u, -v, -1};
  }
}

// The sky cube's small levels (faces of 32 pixels and less) made anew, each pixel the light of the
// whole sky as a surface of that level's roughness gathers it about that direction, in place of
// the 2 x 2 averages (which know nothing of the neighbouring faces: the blurriest level was six
// flat colours with hard edges between them). The lighting reads level roughness x 8 for what a
// surface mirrors and level 8 for the light from all sides, so level L is gathered with the lobe
// of roughness L / 8: weight cos^e about the direction, e = (2 / roughness^4 - 2) / 4 and never
// under 1 (the plain cosine of light from all sides). Gathered from the 16 pixel level in linear
// light, each of its pixels by the piece of sky it covers.
void PrefilterSky(std::vector<std::vector<uint8_t>> levels[6], int width) {
  int from = -1;
  for (size_t l = 0; l < levels[0].size(); ++l)
    if ((width >> l) == 16) from = static_cast<int>(l);
  if (from < 1) return;  // (a cube smaller than that: left as it is)
  struct Sample {
    Vec3 direction;
    float light[3];  // linear
    float covers;    // the piece of sky the pixel covers
  };
  std::vector<Sample> sky;
  sky.reserve(6 * 16 * 16);
  for (int f = 0; f < 6; ++f)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / 8 - 1, v = (static_cast<float>(y) + 0.5f) / 8 - 1;
        const uint8_t* p = &levels[f][static_cast<size_t>(from)][(static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4];
        Sample s;
        s.direction = Normalize(CubeDirection(f, u, v));
        for (int c = 0; c < 3; ++c) s.light[c] = std::pow(static_cast<float>(p[c]) / 255.0f, 2.2f);
        s.covers = 1 / std::pow(1 + u * u + v * v, 1.5f);
        sky.push_back(s);
      }
  for (size_t l = 0; l < levels[0].size(); ++l) {
    const int size = width >> l;
    if (size > 32 || size < 1) continue;
    const float roughness = std::min(static_cast<float>(l) / 8.0f, 1.0f), a2 = roughness * roughness * roughness * roughness;
    const float e = std::max((2 / std::max(a2, 1e-4f) - 2) * 0.25f, 1.0f);
    for (int f = 0; f < 6; ++f)
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
          const float half = static_cast<float>(size) * 0.5f;
          const Vec3 d = Normalize(CubeDirection(f, (static_cast<float>(x) + 0.5f) / half - 1, (static_cast<float>(y) + 0.5f) / half - 1));
          float sum[3] = {0, 0, 0}, weights = 0;
          for (const Sample& s : sky) {
            const float facing = Dot(d, s.direction);
            if (facing <= 0) continue;
            const float w = (e == 1.0f ? facing : std::pow(facing, e)) * s.covers;
            sum[0] += s.light[0] * w;
            sum[1] += s.light[1] * w;
            sum[2] += s.light[2] * w;
            weights += w;
          }
          uint8_t* q = &levels[f][l][(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
          for (int c = 0; c < 3; ++c)
            q[c] = static_cast<uint8_t>(std::min(std::pow(weights > 0 ? sum[c] / weights : 0.0f, 1 / 2.2f) * 255.0f + 0.5f, 255.0f));
        }
  }
}

}  // namespace

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
    std::vector<std::vector<uint8_t>> levels[6];  // per face, per level
    int mips = 0;
    bool ok = true;
    for (int layer = 0; layer < 6 && ok; ++layer) {
      ok = DecodeRgba(image, layer, &face);
      mips = 0;
      for (int size = image.width; ok && size >= 1; size /= 2, ++mips) {
        levels[layer].push_back(face);
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
      const auto t0 = std::chrono::steady_clock::now();
      PrefilterSky(levels, image.width);
      printf("render: sky cube %s: its blurred levels gathered in %.0f ms\n", path.c_str(),
             std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    for (int layer = 0; layer < 6 && ok; ++layer)
      for (const std::vector<uint8_t>& level : levels[layer]) all.insert(all.end(), level.begin(), level.end());
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
