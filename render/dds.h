// Reads .dds texture files as the game's data uses them: DXT1 / DXT3 / DXT5, 32-bit BGRA, 24-bit BGR
// and 8-bit alpha or luminance; 2D textures and cube maps, with or without mip levels.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gfx.h"

namespace render {

struct DdsImage {
  int width = 0, height = 0;
  int mips = 1;
  int layers = 1;  // 6 for a cube map
  bool cube = false;
  gfx::Format format = gfx::Format::Unknown;
  // The images in gfx::TextureDesc order. Points into the file's bytes, or into `converted` when
  // the pixels had to be expanded (24-bit).
  const uint8_t* data = nullptr;
  size_t dataSize = 0;
  std::vector<uint8_t> converted;

  gfx::TextureDesc Desc() const;
};

// `file` must outlive `out` (see DdsImage::data). False with a reason when the file cannot be used.
bool ParseDds(std::string_view file, DdsImage* out, std::string* error);

// The first (largest) image of one layer as plain RGBA, 4 bytes per pixel. Handles the compressed
// formats DXT1 / DXT3 / DXT5 and the 32-bit ones. False for anything else.
bool DecodeRgba(const DdsImage& image, int layer, std::vector<uint8_t>* out);

}  // namespace render
