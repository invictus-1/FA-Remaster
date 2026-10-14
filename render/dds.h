// Reads .dds texture files as the game's data uses them: DXT1 / DXT3 / DXT5, 32-bit BGRA, 24-bit BGR
// and 8-bit alpha or luminance; 2D textures and cube maps, with or without mip levels. And files
// with the newer header ("DX10"), which new art may come in: the same formats named by number, and
// BC7, which keeps colour and normals much better than DXT5 at the same size. A file that says its
// colours are sRGB is taken as plain numbers, like every other texture here: the shaders do that step.
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
// formats DXT1 / DXT3 / DXT5 and the 32-bit ones. False for anything else (BC4, BC5 and BC7 are for
// the graphics card only).
bool DecodeRgba(const DdsImage& image, int layer, std::vector<uint8_t>* out);

}  // namespace render
