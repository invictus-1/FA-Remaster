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

bool DecodeRgba(const DdsImage& image, int layer, std::vector<uint8_t>* out) {
  const int w = image.width, h = image.height;
  if (layer < 0 || layer >= image.layers || !image.data) return false;
  // one layer's bytes: its whole mip chain
  size_t chain = 0;
  for (int m = 0; m < image.mips; ++m) chain += ImageBytes(image.format, std::max(1, w >> m), std::max(1, h >> m));
  const uint8_t* src = image.data + chain * static_cast<size_t>(layer);
  out->assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 255);
  uint8_t* dst = out->data();
  const gfx::Format f = image.format;
  if (f == gfx::Format::RGBA8 || f == gfx::Format::BGRA8) {
    for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i) {
      dst[i * 4 + 0] = src[i * 4 + (f == gfx::Format::BGRA8 ? 2 : 0)];
      dst[i * 4 + 1] = src[i * 4 + 1];
      dst[i * 4 + 2] = src[i * 4 + (f == gfx::Format::BGRA8 ? 0 : 2)];
      dst[i * 4 + 3] = src[i * 4 + 3];
    }
    return true;
  }
  if (f != gfx::Format::BC1 && f != gfx::Format::BC2 && f != gfx::Format::BC3) return false;
  const size_t blockBytes = f == gfx::Format::BC1 ? 8 : 16;
  for (int by = 0; by < (h + 3) / 4; ++by) {
    for (int bx = 0; bx < (w + 3) / 4; ++bx, src += blockBytes) {
      const uint8_t* colorBlock = f == gfx::Format::BC1 ? src : src + 8;
      // the two stored colours (5:6:5 bits) and the two between them
      const uint16_t c0 = static_cast<uint16_t>(colorBlock[0] | colorBlock[1] << 8), c1 = static_cast<uint16_t>(colorBlock[2] | colorBlock[3] << 8);
      uint8_t palette[4][4];
      auto expand = [](uint16_t c, uint8_t* p) {
        p[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        p[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        p[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        p[3] = 255;
      };
      expand(c0, palette[0]);
      expand(c1, palette[1]);
      const bool fourColors = f != gfx::Format::BC1 || c0 > c1;
      for (int k = 0; k < 3; ++k) {
        palette[2][k] = static_cast<uint8_t>(fourColors ? (2 * palette[0][k] + palette[1][k]) / 3 : (palette[0][k] + palette[1][k]) / 2);
        palette[3][k] = static_cast<uint8_t>(fourColors ? (palette[0][k] + 2 * palette[1][k]) / 3 : 0);
      }
      palette[2][3] = 255;
      palette[3][3] = fourColors ? 255 : 0;
      // DXT5's alpha: two stored values and six between them (or four, and 0 and 255)
      uint8_t alphas[8] = {};
      uint64_t alphaBits = 0;
      if (f == gfx::Format::BC3) {
        alphas[0] = src[0];
        alphas[1] = src[1];
        const int steps = alphas[0] > alphas[1] ? 7 : 5;
        for (int k = 1; k < steps; ++k) alphas[k + 1] = static_cast<uint8_t>(((steps - k) * alphas[0] + k * alphas[1]) / steps);
        if (steps == 5) {
          alphas[6] = 0;
          alphas[7] = 255;
        }
        for (int k = 0; k < 6; ++k) alphaBits |= static_cast<uint64_t>(src[2 + k]) << (8 * k);
      }
      const uint32_t bits = static_cast<uint32_t>(colorBlock[4]) | static_cast<uint32_t>(colorBlock[5]) << 8 |
                            static_cast<uint32_t>(colorBlock[6]) << 16 | static_cast<uint32_t>(colorBlock[7]) << 24;
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
          const int x = bx * 4 + px, y = by * 4 + py, at = py * 4 + px;
          if (x >= w || y >= h) continue;
          uint8_t* p = dst + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
          std::memcpy(p, palette[(bits >> (2 * at)) & 3], 4);
          if (f == gfx::Format::BC2) p[3] = static_cast<uint8_t>(((src[at / 2] >> (4 * (at & 1))) & 15) * 17);
          else if (f == gfx::Format::BC3) p[3] = alphas[(alphaBits >> (3 * at)) & 7];
        }
      }
    }
  }
  return true;
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
