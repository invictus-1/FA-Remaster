#include "dds.h"

#include <algorithm>
#include <cstring>

namespace render {
namespace {

uint32_t U32(std::string_view f, size_t at) {
  uint32_t v;
  std::memcpy(&v, f.data() + at, 4);
  return v;
}

// bytes of one image of the mip chain
size_t ImageBytes(gfx::Format format, int width, int height) {
  size_t unit = static_cast<size_t>(gfx::BytesPerPixelOrBlock(format));
  size_t w = static_cast<size_t>(width), h = static_cast<size_t>(height);
  if (gfx::IsBlockCompressed(format)) return ((w + 3) / 4) * ((h + 3) / 4) * unit;
  return w * h * unit;
}

}  // namespace

gfx::TextureDesc DdsImage::Desc() const {
  gfx::TextureDesc d;
  d.width = width;
  d.height = height;
  d.mips = mips;
  d.layers = layers;
  d.cube = cube;
  d.format = format;
  d.data = data;
  d.dataSize = dataSize;
  return d;
}

bool ParseDds(std::string_view file, DdsImage* out, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = what;
    return false;
  };
  if (file.size() < 128 || file.compare(0, 4, "DDS ") != 0) return fail("not a DDS file");
  const int height = static_cast<int>(U32(file, 12)), width = static_cast<int>(U32(file, 16));
  int mips = static_cast<int>(U32(file, 28));
  const uint32_t pfFlags = U32(file, 80), fourcc = U32(file, 84), bits = U32(file, 88);
  const uint32_t rMask = U32(file, 92), aMask = U32(file, 104);
  const uint32_t caps2 = U32(file, 112);
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("bad size");
  if (caps2 & 0x200000) return fail("volume textures are not supported");

  *out = DdsImage{};
  out->width = width;
  out->height = height;
  out->cube = (caps2 & 0x200) != 0;
  out->layers = out->cube ? 6 : 1;
  int sourceBytes = 0;  // per pixel in the file, when it differs from the format's
  if (pfFlags & 4) {    // four-character code
    char cc[5] = {};
    std::memcpy(cc, &fourcc, 4);
    if (!std::strcmp(cc, "DXT1")) out->format = gfx::Format::BC1;
    else if (!std::strcmp(cc, "DXT2") || !std::strcmp(cc, "DXT3")) out->format = gfx::Format::BC2;
    else if (!std::strcmp(cc, "DXT4") || !std::strcmp(cc, "DXT5")) out->format = gfx::Format::BC3;
    else if (!std::strcmp(cc, "ATI1") || !std::strcmp(cc, "BC4U")) out->format = gfx::Format::BC4;
    else if (!std::strcmp(cc, "ATI2") || !std::strcmp(cc, "BC5U")) out->format = gfx::Format::BC5;
    else return fail("unsupported compressed format");
  } else if (bits == 32) {
    out->format = rMask == 0xff ? gfx::Format::RGBA8 : gfx::Format::BGRA8;
  } else if (bits == 24) {
    out->format = rMask == 0xff ? gfx::Format::RGBA8 : gfx::Format::BGRA8;
    sourceBytes = 3;
  } else if (bits == 8) {
    out->format = aMask == 0xff ? gfx::Format::A8 : gfx::Format::R8;
  } else {
    return fail("unsupported pixel format");
  }

  // some files say 0 mips, some claim more than they hold: keep the levels that are really there
  const int layers = out->layers;
  mips = std::max(1, mips);
  const size_t available = file.size() - 128;
  auto chainBytes = [&](int levels, bool source) {
    size_t n = 0;
    for (int m = 0; m < levels; ++m) {
      int w = std::max(1, width >> m), h = std::max(1, height >> m);
      n += source && sourceBytes ? static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(sourceBytes)
                                 : ImageBytes(out->format, w, h);
    }
    return n;
  };
  while (mips > 1 && ((std::max(width, height) >> (mips - 1)) == 0 || chainBytes(mips, true) * static_cast<size_t>(layers) > available)) --mips;
  if (chainBytes(mips, true) * static_cast<size_t>(layers) > available) return fail("file is cut short");
  out->mips = mips;

  const uint8_t* src = reinterpret_cast<const uint8_t*>(file.data()) + 128;
  if (!sourceBytes) {
    out->data = src;
    out->dataSize = chainBytes(mips, false) * static_cast<size_t>(layers);
    return true;
  }
  // 24-bit: add an opaque alpha byte
  const size_t pixels = chainBytes(mips, true) * static_cast<size_t>(layers) / 3;
  out->converted.resize(pixels * 4);
  uint8_t* dst = out->converted.data();
  for (size_t i = 0; i < pixels; ++i, src += 3, dst += 4) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 255;
  }
  out->data = out->converted.data();
  out->dataSize = out->converted.size();
  return true;
}

}  // namespace render
