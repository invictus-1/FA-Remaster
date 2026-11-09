#include "png.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <zlib.h>

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

bool ReadPngGrey16(const std::string& file, int* width, int* height, std::vector<uint16_t>* out, std::string* why) {
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const uint8_t* p = reinterpret_cast<const uint8_t*>(file.data());
  const size_t size = file.size();
  auto word = [&](size_t at) { return static_cast<uint32_t>(p[at]) << 24 | static_cast<uint32_t>(p[at + 1]) << 16 | static_cast<uint32_t>(p[at + 2]) << 8 | p[at + 3]; };
  if (size < 8 + 25 || std::memcmp(p, kSignature, 8) != 0) {
    *why = "not a PNG";
    return false;
  }
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> packed;
  bool head = false, end = false;
  for (size_t at = 8; at + 12 <= size && !end;) {
    const uint32_t length = word(at);
    if (length > size - at - 12) {
      *why = "a PNG chunk longer than the file";
      return false;
    }
    const uint8_t* name = p + at + 4;
    const uint8_t* body = p + at + 8;
    if (!std::memcmp(name, "IHDR", 4)) {
      if (length != 13) {
        *why = "a PNG header of the wrong length";
        return false;
      }
      w = word(at + 8);
      h = word(at + 12);
      // bit depth 16, colour type 0 (grey), compression 0, filter 0, not interlaced
      if (body[8] != 16 || body[9] != 0 || body[10] != 0 || body[11] != 0 || body[12] != 0) {
        *why = "not a 16-bit grey PNG without interlace";
        return false;
      }
      if (!w || !h || w > 16384 || h > 16384) {
        *why = "a PNG of no size, or more than 16384 across or down";
        return false;
      }
      head = true;
    } else if (!std::memcmp(name, "IDAT", 4)) {
      packed.insert(packed.end(), body, body + length);
    } else if (!std::memcmp(name, "IEND", 4)) {
      end = true;
    }
    at += 12 + static_cast<size_t>(length);
  }
  if (!head || packed.empty()) {
    *why = "a PNG without a header or without a picture";
    return false;
  }
  const size_t row = static_cast<size_t>(w) * 2, all = (row + 1) * h;
  std::vector<uint8_t> raw(all);
  z_stream z = {};  // (inflate itself: the tree's zlib is without its one-call helper)
  z.next_in = packed.data();
  z.avail_in = static_cast<uInt>(packed.size());
  z.next_out = raw.data();
  z.avail_out = static_cast<uInt>(all);
  bool unpacked = inflateInit(&z) == Z_OK;
  if (unpacked) {
    unpacked = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == all;
    inflateEnd(&z);
  }
  if (!unpacked) {
    *why = "a PNG whose picture cannot be unpacked";
    return false;
  }
  // each row: its filter, then its bytes as differences from the value to the left (2 bytes back), the row above, or both
  std::vector<uint8_t> above(row, 0);
  out->resize(static_cast<size_t>(w) * h);
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* line = raw.data() + (row + 1) * y;
    const uint8_t filter = line[0];
    uint8_t* d = line + 1;
    if (filter > 4) {
      *why = "a PNG row with an unknown filter";
      return false;
    }
    for (size_t i = 0; i < row; ++i) {
      const int a = i >= 2 ? d[i - 2] : 0, b = above[i], c = i >= 2 ? above[i - 2] : 0;
      int add = 0;
      if (filter == 1) add = a;
      else if (filter == 2) add = b;
      else if (filter == 3) add = (a + b) / 2;
      else if (filter == 4) {
        const int e = a + b - c, ea = std::abs(e - a), eb = std::abs(e - b), ec = std::abs(e - c);
        add = ea <= eb && ea <= ec ? a : eb <= ec ? b : c;
      }
      d[i] = static_cast<uint8_t>(d[i] + add);
    }
    std::memcpy(above.data(), d, row);
    uint16_t* to = out->data() + static_cast<size_t>(w) * y;
    for (uint32_t x = 0; x < w; ++x) to[x] = static_cast<uint16_t>(d[2 * x] << 8 | d[2 * x + 1]);
  }
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

}  // namespace render
