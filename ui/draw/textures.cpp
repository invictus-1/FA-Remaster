// Texture files to pixels: .dds (DXT1, DXT3, DXT5 and uncompressed formats), top mip level only,
// and .png (FAF's newer interface pictures: 8 and 16 bits a channel, grey, colour and palette,
// with or without alpha; interlaced files are not read).
#include <cstdlib>
#include <cstring>
#include <map>

#include <zlib.h>

#include "core/log.h"
#include "core/vfs.h"
#include "draw/draw.h"

namespace moho::ui {
namespace {

uint32_t Le32(const std::string& d, size_t o) {
  return static_cast<uint8_t>(d[o]) | static_cast<uint8_t>(d[o + 1]) << 8 | static_cast<uint8_t>(d[o + 2]) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(d[o + 3])) << 24;
}
uint32_t Le16(const std::string& d, size_t o) { return static_cast<uint8_t>(d[o]) | static_cast<uint8_t>(d[o + 1]) << 8; }

inline uint32_t Expand565(uint32_t c) {
  uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
inline uint32_t Mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t div) {
  uint32_t out = 0;
  for (int s = 0; s < 24; s += 8) out |= ((((a >> s) & 0xff) * wa + ((b >> s) & 0xff) * wb) / div) << s;
  return out;
}

// One 4x4 colour block (DXT1 layout). `alwaysFour`: DXT3/5 colour blocks never use the 3-colour mode.
void ColorBlock(const std::string& d, size_t o, bool alwaysFour, uint32_t out[16]) {
  uint32_t c0 = Le16(d, o), c1 = Le16(d, o + 2), bits = Le32(d, o + 4);
  uint32_t p[4];
  p[0] = 0xff000000u | Expand565(c0);
  p[1] = 0xff000000u | Expand565(c1);
  if (c0 > c1 || alwaysFour) {
    p[2] = 0xff000000u | Mix(p[0], p[1], 2, 1, 3);
    p[3] = 0xff000000u | Mix(p[0], p[1], 1, 2, 3);
  } else {
    p[2] = 0xff000000u | Mix(p[0], p[1], 1, 1, 2);
    p[3] = 0;  // transparent black
  }
  for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (2 * i)) & 3];
}

bool DecodeDxt(const std::string& d, size_t off, int w, int h, int kind, Image& img) {
  size_t blockBytes = kind == 1 ? 8 : 16;
  size_t bw = static_cast<size_t>((w + 3) / 4), bh = static_cast<size_t>((h + 3) / 4);
  if (off + bw * bh * blockBytes > d.size()) return false;
  for (size_t by = 0; by < bh; ++by) {
    for (size_t bx = 0; bx < bw; ++bx) {
      size_t o = off + (by * bw + bx) * blockBytes;
      uint32_t px[16];
      uint8_t alpha[16];
      if (kind == 1) {
        ColorBlock(d, o, false, px);
      } else {
        ColorBlock(d, o + 8, true, px);
        if (kind == 3) {  // 4 bits of alpha per pixel
          for (int i = 0; i < 16; ++i) {
            uint32_t a4 = (static_cast<uint8_t>(d[o + static_cast<size_t>(i / 2)]) >> (4 * (i & 1))) & 15;
            alpha[i] = static_cast<uint8_t>(a4 * 17);
          }
        } else {  // DXT5: two alpha values and 3-bit indices between them
          uint32_t a0 = static_cast<uint8_t>(d[o]), a1 = static_cast<uint8_t>(d[o + 1]);
          uint32_t pal[8] = {a0, a1};
          if (a0 > a1) {
            for (uint32_t i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
          } else {
            for (uint32_t i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            pal[6] = 0;
            pal[7] = 255;
          }
          uint64_t bits = 0;
          for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(static_cast<uint8_t>(d[o + 2 + static_cast<size_t>(i)])) << (8 * i);
          for (int i = 0; i < 16; ++i) alpha[i] = static_cast<uint8_t>(pal[(bits >> (3 * i)) & 7]);
        }
        for (int i = 0; i < 16; ++i) px[i] = (px[i] & 0x00ffffffu) | static_cast<uint32_t>(alpha[i]) << 24;
      }
      for (int i = 0; i < 16; ++i) {
        size_t x = bx * 4 + static_cast<size_t>(i % 4), y = by * 4 + static_cast<size_t>(i / 4);
        if (x < static_cast<size_t>(w) && y < static_cast<size_t>(h)) img.pixels[y * static_cast<size_t>(w) + x] = px[i];
      }
    }
  }
  return true;
}

// A channel given by a bit mask, scaled to 8 bits.
uint32_t Channel(uint32_t v, uint32_t mask) {
  if (!mask) return 0;
  int shift = 0, bits = 0;
  while (!((mask >> shift) & 1)) ++shift;
  while ((mask >> (shift + bits)) & 1) ++bits;
  uint32_t c = (v & mask) >> shift;
  return bits >= 8 ? c >> (bits - 8) : c * 255 / ((1u << bits) - 1);
}

bool DecodeDds(const std::string& d, Image& img) {
  if (d.size() < 128 || d.compare(0, 4, "DDS ") != 0) return false;
  int h = static_cast<int>(Le32(d, 12)), w = static_cast<int>(Le32(d, 16));
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  uint32_t pfFlags = Le32(d, 80), fourCC = Le32(d, 84), bitCount = Le32(d, 88);
  uint32_t rm = Le32(d, 92), gm = Le32(d, 96), bm = Le32(d, 100), am = Le32(d, 104);
  img.width = w;
  img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  if (pfFlags & 4) {  // DDPF_FOURCC
    int kind = fourCC == 0x31545844 ? 1 : (fourCC == 0x33545844 || fourCC == 0x32545844) ? 3
             : (fourCC == 0x35545844 || fourCC == 0x34545844) ? 5 : 0;
    return kind && DecodeDxt(d, 128, w, h, kind, img);
  }
  size_t bytes = bitCount / 8;
  if (bytes < 1 || bytes > 4 || 128 + static_cast<size_t>(w) * static_cast<size_t>(h) * bytes > d.size()) return false;
  bool hasAlpha = (pfFlags & 1) && am;       // DDPF_ALPHAPIXELS
  bool luminance = (pfFlags & 0x20000) != 0;  // DDPF_LUMINANCE
  bool alphaOnly = (pfFlags & 2) && !rm;      // DDPF_ALPHA
  for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
    uint32_t v = 0;
    for (size_t k = 0; k < bytes; ++k) v |= static_cast<uint32_t>(static_cast<uint8_t>(d[128 + i * bytes + k])) << (8 * k);
    uint32_t a = hasAlpha || alphaOnly ? Channel(v, am) : 255;
    uint32_t r = alphaOnly ? 255 : Channel(v, rm);
    uint32_t g = alphaOnly ? 255 : luminance ? r : Channel(v, gm);
    uint32_t b = alphaOnly ? 255 : luminance ? r : Channel(v, bm);
    img.pixels[i] = a << 24 | r << 16 | g << 8 | b;
  }
  return true;
}

uint32_t Be32(const std::string& d, size_t o) {
  return static_cast<uint32_t>(static_cast<uint8_t>(d[o])) << 24 | static_cast<uint8_t>(d[o + 1]) << 16 |
         static_cast<uint8_t>(d[o + 2]) << 8 | static_cast<uint8_t>(d[o + 3]);
}

// A PNG file: its chunks (IHDR, PLTE, tRNS, the IDATs), the zlib stream unpacked, each row's
// filter undone, the samples brought to 8 bits and to 0xAARRGGBB.
bool DecodePng(const std::string& d, Image& img) {
  static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (d.size() < 33 || std::memcmp(d.data(), kSignature, 8) != 0) return false;
  int w = 0, h = 0, depth = 0, kind = 0, interlace = 0;
  std::string packed, palette, paletteAlpha;
  int greyKey = -1;
  for (size_t at = 8; at + 12 <= d.size();) {
    uint32_t length = Be32(d, at);
    if (length > d.size() - at - 12) return false;
    std::string type = d.substr(at + 4, 4);
    size_t body = at + 8;
    if (type == "IHDR" && length >= 13) {
      w = static_cast<int>(Be32(d, body)), h = static_cast<int>(Be32(d, body + 4));
      depth = static_cast<uint8_t>(d[body + 8]), kind = static_cast<uint8_t>(d[body + 9]);
      interlace = static_cast<uint8_t>(d[body + 12]);
    } else if (type == "PLTE") {
      palette = d.substr(body, length);
    } else if (type == "tRNS") {
      if (kind == 3) paletteAlpha = d.substr(body, length);
      else if (kind == 0 && length >= 2) greyKey = static_cast<uint8_t>(d[body]) << 8 | static_cast<uint8_t>(d[body + 1]);
    } else if (type == "IDAT") {
      packed.append(d, body, length);
    } else if (type == "IEND") {
      break;
    }
    at = body + length + 4;
  }
  int channels = kind == 0 ? 1 : kind == 2 ? 3 : kind == 3 ? 1 : kind == 4 ? 2 : kind == 6 ? 4 : 0;
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !channels || interlace != 0) return false;
  if (depth != 8 && depth != 16 && !((kind == 0 || kind == 3) && (depth == 1 || depth == 2 || depth == 4))) return false;
  size_t bitsPerPixel = static_cast<size_t>(channels) * depth, rowBytes = (static_cast<size_t>(w) * bitsPerPixel + 7) / 8;
  size_t step = std::max<size_t>(1, bitsPerPixel / 8);  // the filter's "pixel to the left", in bytes
  std::string raw((rowBytes + 1) * static_cast<size_t>(h), static_cast<char>(0));
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return false;
  zs.next_in = reinterpret_cast<Bytef*>(packed.data());
  zs.avail_in = static_cast<uInt>(packed.size());
  zs.next_out = reinterpret_cast<Bytef*>(raw.data());
  zs.avail_out = static_cast<uInt>(raw.size());
  int rc = inflate(&zs, Z_FINISH);
  bool whole = zs.total_out == raw.size();
  inflateEnd(&zs);
  if ((rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) || !whole) return false;
  // the filters: none, left, up, average, Paeth
  for (int y = 0; y < h; ++y) {
    uint8_t* row = reinterpret_cast<uint8_t*>(raw.data()) + static_cast<size_t>(y) * (rowBytes + 1);
    const uint8_t* up = y ? row - rowBytes : nullptr;  // (the row before, after its own filter byte)
    uint8_t filter = row[0];
    uint8_t* cur = row + 1;
    for (size_t x = 0; x < rowBytes; ++x) {
      int a = x >= step ? cur[x - step] : 0, b = up ? up[x] : 0, c = (up && x >= step) ? up[x - step] : 0;
      int add = 0;
      switch (filter) {
        case 1: add = a; break;
        case 2: add = b; break;
        case 3: add = (a + b) / 2; break;
        case 4: {
          int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          add = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
          break;
        }
        default: break;
      }
      cur[x] = static_cast<uint8_t>(cur[x] + add);
    }
  }
  img.width = w, img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  const int bytesPerSample = depth == 16 ? 2 : 1;
  for (int y = 0; y < h; ++y) {
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(raw.data()) + static_cast<size_t>(y) * (rowBytes + 1) + 1;
    for (int x = 0; x < w; ++x) {
      uint32_t r = 0, g = 0, b = 0, a = 255;
      if (depth < 8) {  // grey or palette, several pixels to a byte
        int shift = 8 - depth - (x * depth) % 8;
        uint32_t v = (cur[(static_cast<size_t>(x) * depth) / 8] >> shift) & ((1u << depth) - 1);
        if (kind == 3) {
          if (v * 3 + 2 < palette.size()) r = static_cast<uint8_t>(palette[v * 3]), g = static_cast<uint8_t>(palette[v * 3 + 1]), b = static_cast<uint8_t>(palette[v * 3 + 2]);
          if (v < paletteAlpha.size()) a = static_cast<uint8_t>(paletteAlpha[v]);
        } else {
          if (greyKey == static_cast<int>(v)) a = 0;
          r = g = b = v * 255 / ((1u << depth) - 1);
        }
      } else {
        const uint8_t* s = cur + static_cast<size_t>(x) * channels * bytesPerSample;
        auto sample = [&](int i) { return static_cast<uint32_t>(s[i * bytesPerSample]); };  // (16 bits: the high byte)
        switch (kind) {
          case 0: r = g = b = sample(0); if (greyKey >= 0 && greyKey == static_cast<int>(depth == 16 ? (s[0] << 8 | s[1]) : s[0])) a = 0; break;
          case 2: r = sample(0), g = sample(1), b = sample(2); break;
          case 3: {
            uint32_t v = s[0];
            if (v * 3 + 2 < palette.size()) r = static_cast<uint8_t>(palette[v * 3]), g = static_cast<uint8_t>(palette[v * 3 + 1]), b = static_cast<uint8_t>(palette[v * 3 + 2]);
            if (v < paletteAlpha.size()) a = static_cast<uint8_t>(paletteAlpha[v]);
            break;
          }
          case 4: r = g = b = sample(0), a = sample(1); break;
          default: r = sample(0), g = sample(1), b = sample(2), a = sample(3); break;
        }
      }
      img.pixels[static_cast<size_t>(y) * w + x] = a << 24 | r << 16 | g << 8 | b;
    }
  }
  return true;
}

}  // namespace

bool DecodeImage(const std::string& data, Image& out) { return DecodeDds(data, out) || DecodePng(data, out); }

const Image& LoadTexture(const Vfs* vfs, const std::string& vpath) {
  static std::map<std::string, Image> cache;
  std::string key = Vfs::Normalize(vpath);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  Image img;
  auto data = vfs->ReadFile(key);
  if (!data || !DecodeImage(*data, img)) {
    if (data) Logf(LogLevel::Debug, "moho64ui: texture format not read yet: %s", vpath.c_str());
    img = Image{};
  }
  return cache.emplace(key, std::move(img)).first->second;
}

}  // namespace moho::ui
