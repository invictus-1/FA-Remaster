#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "core/hostfs.h"
#include "core/log.h"
#include "draw/draw.h"
#include "zlib.h"

namespace moho::ui {
namespace {

inline uint32_t A(uint32_t c) { return c >> 24; }
inline uint32_t R(uint32_t c) { return (c >> 16) & 0xff; }
inline uint32_t G(uint32_t c) { return (c >> 8) & 0xff; }
inline uint32_t B(uint32_t c) { return c & 0xff; }
inline uint32_t Mul(uint32_t a, uint32_t b) { return (a * b + 127) / 255; }

// Source over destination; the buffer keeps an alpha channel too.
inline void Blend(uint32_t& dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  if (a == 0) return;
  if (a == 255) {
    dst = 0xff000000u | r << 16 | g << 8 | b;
    return;
  }
  uint32_t ia = 255 - a;
  uint32_t orr = Mul(r, a) + Mul(R(dst), ia), og = Mul(g, a) + Mul(G(dst), ia), ob = Mul(b, a) + Mul(B(dst), ia);
  uint32_t oa = a + Mul(A(dst), ia);
  dst = oa << 24 | orr << 16 | og << 8 | ob;
}

// The whole pixels whose centres lie in [a, b).
inline int FirstPixel(float a) { return static_cast<int>(std::ceil(a - 0.5f)); }

struct Name {
  const char* name;
  uint32_t color;
};
const Name kNames[] = {
    {"white", 0xffffffff}, {"black", 0xff000000}, {"red", 0xffff0000},    {"green", 0xff008000},
    {"lime", 0xff00ff00},  {"blue", 0xff0000ff},  {"yellow", 0xffffff00}, {"cyan", 0xff00ffff},
    {"aqua", 0xff00ffff},  {"magenta", 0xffff00ff}, {"fuchsia", 0xffff00ff}, {"gray", 0xff808080},
    {"grey", 0xff808080},  {"silver", 0xffc0c0c0}, {"orange", 0xffffa500}, {"purple", 0xff800080},
    {"transparent", 0x00000000},
};

}  // namespace

uint32_t ParseColor(const std::string& text) {
  bool hex = (text.size() == 8 || text.size() == 6);
  for (char c : text) hex = hex && std::isxdigit(static_cast<unsigned char>(c));
  if (hex) {
    uint32_t v = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
    return text.size() == 6 ? 0xff000000u | v : v;
  }
  std::string lower = hostfs::ToLower(text);
  for (const auto& n : kNames)
    if (lower == n.name) return n.color;
  return 0xffffffff;
}

uint32_t WithAlpha(uint32_t color, float alpha) {
  float a = static_cast<float>(A(color)) * std::max(0.0f, std::min(1.0f, alpha));
  return (color & 0x00ffffffu) | static_cast<uint32_t>(a + 0.5f) << 24;
}

SoftwareTarget::SoftwareTarget(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u) {
  ClearClip();
}

void SoftwareTarget::Clear(uint32_t color) { std::fill(pixels_.begin(), pixels_.end(), color); }

void SoftwareTarget::SetClip(const Rect& r) {
  cx0_ = std::max(0, FirstPixel(r.x0));
  cy0_ = std::max(0, FirstPixel(r.y0));
  cx1_ = std::min(width_, FirstPixel(r.x1));
  cy1_ = std::min(height_, FirstPixel(r.y1));
}

void SoftwareTarget::ClearClip() {
  cx0_ = cy0_ = 0;
  cx1_ = width_;
  cy1_ = height_;
}

void SoftwareTarget::FillRect(const Rect& d, uint32_t color) {
  int x0 = std::max(cx0_, FirstPixel(d.x0)), x1 = std::min(cx1_, FirstPixel(d.x1));
  int y0 = std::max(cy0_, FirstPixel(d.y0)), y1 = std::min(cy1_, FirstPixel(d.y1));
  uint32_t a = A(color), r = R(color), g = G(color), b = B(color);
  if (a == 0) return;
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = &pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    for (int x = x0; x < x1; ++x) Blend(row[x], r, g, b, a);
  }
}

void SoftwareTarget::DrawImage(const Image& img, const Rect& s, const Rect& d, uint32_t color) {
  if (!img.valid() || d.x1 <= d.x0 || d.y1 <= d.y0 || A(color) == 0) return;
  int x0 = std::max(cx0_, FirstPixel(d.x0)), x1 = std::min(cx1_, FirstPixel(d.x1));
  int y0 = std::max(cy0_, FirstPixel(d.y0)), y1 = std::min(cy1_, FirstPixel(d.y1));
  if (x0 >= x1 || y0 >= y1) return;
  float sx = (s.x1 - s.x0) / (d.x1 - d.x0), sy = (s.y1 - s.y0) / (d.y1 - d.y0);
  uint32_t ca = A(color), cr = R(color), cg = G(color), cb = B(color);
  bool plain = color == 0xffffffffu;
  // One texel per pixel and whole-texel offsets: copy texels; otherwise filter between four.
  bool exact = std::fabs(sx - 1.0f) < 1e-4f && std::fabs(sy - 1.0f) < 1e-4f &&
               std::fabs((s.x0 - d.x0) - std::nearbyint(s.x0 - d.x0)) < 1e-3f &&
               std::fabs((s.y0 - d.y0) - std::nearbyint(s.y0 - d.y0)) < 1e-3f;
  int lastX = img.width - 1, lastY = img.height - 1;
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = &pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    float v = s.y0 + (static_cast<float>(y) + 0.5f - d.y0) * sy;  // in texels; a texel's centre is at +0.5
    for (int x = x0; x < x1; ++x) {
      float u = s.x0 + (static_cast<float>(x) + 0.5f - d.x0) * sx;
      uint32_t ta, tr, tg, tb;
      if (exact) {
        int tx = std::max(0, std::min(lastX, static_cast<int>(std::floor(u))));
        int ty = std::max(0, std::min(lastY, static_cast<int>(std::floor(v))));
        uint32_t t = img.pixels[static_cast<size_t>(ty) * static_cast<size_t>(img.width) + static_cast<size_t>(tx)];
        ta = A(t), tr = R(t), tg = G(t), tb = B(t);
      } else {
        // The four texels around the sample point stay inside the source rectangle, so a texture's
        // border ring does not leak in.
        float fu = u - 0.5f, fv = v - 0.5f;
        int tx = static_cast<int>(std::floor(fu)), ty = static_cast<int>(std::floor(fv));
        float wx = fu - static_cast<float>(tx), wy = fv - static_cast<float>(ty);
        int minX = std::max(0, static_cast<int>(std::floor(s.x0))), maxX = std::min(lastX, static_cast<int>(std::ceil(s.x1)) - 1);
        int minY = std::max(0, static_cast<int>(std::floor(s.y0))), maxY = std::min(lastY, static_cast<int>(std::ceil(s.y1)) - 1);
        int xa = std::max(minX, std::min(maxX, tx)), xb = std::max(minX, std::min(maxX, tx + 1));
        int ya = std::max(minY, std::min(maxY, ty)), yb = std::max(minY, std::min(maxY, ty + 1));
        const uint32_t* p = img.pixels.data();
        size_t w = static_cast<size_t>(img.width);
        uint32_t t00 = p[static_cast<size_t>(ya) * w + static_cast<size_t>(xa)], t10 = p[static_cast<size_t>(ya) * w + static_cast<size_t>(xb)];
        uint32_t t01 = p[static_cast<size_t>(yb) * w + static_cast<size_t>(xa)], t11 = p[static_cast<size_t>(yb) * w + static_cast<size_t>(xb)];
        // Filter with alpha as weight, so the colour of transparent texels does not bleed in.
        float w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
        float a00 = w00 * static_cast<float>(A(t00)), a10 = w10 * static_cast<float>(A(t10));
        float a01 = w01 * static_cast<float>(A(t01)), a11 = w11 * static_cast<float>(A(t11));
        float fa = a00 + a10 + a01 + a11;
        if (fa <= 0.0f) continue;
        ta = static_cast<uint32_t>(fa + 0.5f);
        tr = static_cast<uint32_t>((a00 * static_cast<float>(R(t00)) + a10 * static_cast<float>(R(t10)) + a01 * static_cast<float>(R(t01)) + a11 * static_cast<float>(R(t11))) / fa + 0.5f);
        tg = static_cast<uint32_t>((a00 * static_cast<float>(G(t00)) + a10 * static_cast<float>(G(t10)) + a01 * static_cast<float>(G(t01)) + a11 * static_cast<float>(G(t11))) / fa + 0.5f);
        tb = static_cast<uint32_t>((a00 * static_cast<float>(B(t00)) + a10 * static_cast<float>(B(t10)) + a01 * static_cast<float>(B(t01)) + a11 * static_cast<float>(B(t11))) / fa + 0.5f);
      }
      if (!plain) ta = Mul(ta, ca), tr = Mul(tr, cr), tg = Mul(tg, cg), tb = Mul(tb, cb);
      Blend(row[x], tr, tg, tb, ta);
    }
  }
}

void SoftwareTarget::DrawImageTiled(const Image& img, const Rect& s, const Rect& d, uint32_t color) {
  float tw = s.x1 - s.x0, th = s.y1 - s.y0;
  if (!img.valid() || tw <= 0 || th <= 0) return;
  int saved[4] = {cx0_, cy0_, cx1_, cy1_};
  cx0_ = std::max(cx0_, FirstPixel(d.x0));
  cy0_ = std::max(cy0_, FirstPixel(d.y0));
  cx1_ = std::min(cx1_, FirstPixel(d.x1));
  cy1_ = std::min(cy1_, FirstPixel(d.y1));
  for (float y = d.y0; y < d.y1; y += th)
    for (float x = d.x0; x < d.x1; x += tw) DrawImage(img, s, Rect{x, y, x + tw, y + th}, color);
  cx0_ = saved[0], cy0_ = saved[1], cx1_ = saved[2], cy1_ = saved[3];
}

void SoftwareTarget::DrawGlyph(const GlyphMask& gl, int x, int y, uint32_t color) {
  uint32_t ca = A(color), r = R(color), g = G(color), b = B(color);
  if (ca == 0) return;
  for (int gy = 0; gy < gl.height; ++gy) {
    int py = y + gy;
    if (py < cy0_ || py >= cy1_) continue;
    uint32_t* row = &pixels_[static_cast<size_t>(py) * static_cast<size_t>(width_)];
    const uint8_t* cov = &gl.coverage[static_cast<size_t>(gy) * static_cast<size_t>(gl.width)];
    for (int gx = 0; gx < gl.width; ++gx) {
      int px = x + gx;
      if (px < cx0_ || px >= cx1_ || !cov[gx]) continue;
      Blend(row[px], r, g, b, Mul(cov[gx], ca));
    }
  }
}

// A PNG with the pixels stored uncompressed (deflate "stored" blocks): simple, and every viewer reads it.
bool SoftwareTarget::SavePng(const std::string& hostPath) const {
  auto be32 = [](std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>(v >> (8 * i));
  };
  auto chunk = [&](std::string& out, const char* type, const std::string& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    std::string body = std::string(type, 4) + data;
    out += body;
    be32(out, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
  };
  std::string raw;  // filter byte 0 + RGB per row
  raw.reserve(static_cast<size_t>(height_) * (static_cast<size_t>(width_) * 3 + 1));
  for (int y = 0; y < height_; ++y) {
    raw += '\0';
    for (int x = 0; x < width_; ++x) {
      uint32_t c = pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
      raw += static_cast<char>(R(c));
      raw += static_cast<char>(G(c));
      raw += static_cast<char>(B(c));
    }
  }
  std::string z = "\x78\x01";
  for (size_t off = 0; off < raw.size();) {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    z += static_cast<char>(off + n == raw.size() ? 1 : 0);
    z += static_cast<char>(n & 0xff);
    z += static_cast<char>(n >> 8);
    z += static_cast<char>(~n & 0xff);
    z += static_cast<char>((~n >> 8) & 0xff);
    z.append(raw, off, n);
    off += n;
  }
  be32(z, static_cast<uint32_t>(adler32(1, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uInt>(raw.size()))));
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  be32(ihdr, static_cast<uint32_t>(width_));
  be32(ihdr, static_cast<uint32_t>(height_));
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bits, RGB
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", "");
  return hostfs::WriteFile(hostPath, out);
}

}  // namespace moho::ui
