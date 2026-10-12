#include "png.h"

#include <cstdio>
#include <vector>

namespace render {
namespace {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void Chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  Put32(out, static_cast<uint32_t>(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  Put32(out, Crc32(out.data() + start, out.size() - start));
}

}  // namespace

bool WritePng(const std::string& path, const uint8_t* rgba, int width, int height) {
  if (width <= 0 || height <= 0) return false;
  // the raw image: per row a filter byte (0 = none) and the RGB pixels
  const size_t row = 1 + static_cast<size_t>(width) * 3;
  std::vector<uint8_t> raw(row * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    uint8_t* d = raw.data() + row * static_cast<size_t>(y);
    const uint8_t* s = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    *d++ = 0;
    for (int x = 0; x < width; ++x, s += 4) {
      *d++ = s[0];
      *d++ = s[1];
      *d++ = s[2];
    }
  }
  // zlib stream of stored blocks (at most 65535 bytes each)
  std::vector<uint8_t> z;
  z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  z.push_back(0x78);
  z.push_back(0x01);
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size();) {
    size_t n = raw.size() - at < 65535 ? raw.size() - at : 65535;
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    for (size_t i = at; i < at + n; ++i) {  // Adler-32
      a = (a + raw[i]) % 65521;
      b = (b + a) % 65521;
    }
    at += n;
  }
  Put32(z, (b << 16) | a);

  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> header;
  Put32(header, static_cast<uint32_t>(width));
  Put32(header, static_cast<uint32_t>(height));
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB
  Chunk(out, "IHDR", header);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  return fclose(f) == 0 && ok;
}

}  // namespace render
