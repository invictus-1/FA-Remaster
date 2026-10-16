#include <algorithm>
#include <atomic>
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
// The colour names scripts may give (case does not matter): the original's table, FA exe 0xdff690, 141
// names with "transparent" (and "grey", ours, as "gray").
const Name kNames[] = {
    {"aliceblue", 0xfff0f8ff}, {"antiquewhite", 0xfffaebd7}, {"aqua", 0xff00ffff}, {"aquamarine", 0xff7fffd4},
    {"azure", 0xfff0ffff}, {"beige", 0xfff5f5dc}, {"bisque", 0xffffe4c4}, {"black", 0xff000000},
    {"blanchedalmond", 0xffffebcd}, {"blue", 0xff0000ff}, {"blueviolet", 0xff8a2be2}, {"brown", 0xffa52a2a},
    {"burlywood", 0xffdeb887}, {"cadetblue", 0xff5f9ea0}, {"chartreuse", 0xff7fff00}, {"chocolate", 0xffd2691e},
    {"coral", 0xffff7f50}, {"cornflowerblue", 0xff6495ed}, {"cornsilk", 0xfffff8dc}, {"crimson", 0xffdc143c},
    {"cyan", 0xff00ffff}, {"darkblue", 0xff00008b}, {"darkcyan", 0xff008b8b}, {"darkgoldenrod", 0xffb8860b},
    {"darkgray", 0xffa9a9a9}, {"darkgreen", 0xff006400}, {"darkkhaki", 0xffbdb76b}, {"darkmagenta", 0xff8b008b},
    {"darkolivegreen", 0xff556b2f}, {"darkorange", 0xffff8c00}, {"darkorchid", 0xff9932cc}, {"darkred", 0xff8b0000},
    {"darksalmon", 0xffe9967a}, {"darkseagreen", 0xff8fbc8b}, {"darkslateblue", 0xff483d8b},
    {"darkslategray", 0xff2f4f4f}, {"darkturquoise", 0xff00ced1}, {"darkviolet", 0xff9400d3},
    {"deeppink", 0xffff1493}, {"deepskyblue", 0xff00bfff}, {"dimgray", 0xff696969}, {"dodgerblue", 0xff1e90ff},
    {"firebrick", 0xffb22222}, {"floralwhite", 0xfffffaf0}, {"forestgreen", 0xff228b22}, {"fuchsia", 0xffff00ff},
    {"gainsboro", 0xffdcdcdc}, {"ghostwhite", 0xfff8f8ff}, {"gold", 0xffffd700}, {"goldenrod", 0xffdaa520},
    {"gray", 0xff808080}, {"green", 0xff008000}, {"greenyellow", 0xffadff2f}, {"honeydew", 0xfff0fff0},
    {"hotpink", 0xffff69b4}, {"indianred", 0xffcd5c5c}, {"indigo", 0xff4b0082}, {"ivory", 0xfffffff0},
    {"khaki", 0xfff0e68c}, {"lavender", 0xffe6e6fa}, {"lavenderblush", 0xfffff0f5}, {"lawngreen", 0xff7cfc00},
    {"lemonchiffon", 0xfffffacd}, {"lightblue", 0xffadd8e6}, {"lightcoral", 0xfff08080}, {"lightcyan", 0xffe0ffff},
    {"lightgoldenrodyellow", 0xfffafad2}, {"lightgray", 0xffd3d3d3}, {"lightgreen", 0xff90ee90},
    {"lightpink", 0xffffb6c1}, {"lightsalmon", 0xffffa07a}, {"lightseagreen", 0xff20b2aa},
    {"lightskyblue", 0xff87cefa}, {"lightslategray", 0xff778899}, {"lightsteelblue", 0xffb0c4de},
    {"lightyellow", 0xffffffe0}, {"lime", 0xff00ff00}, {"limegreen", 0xff32cd32}, {"linen", 0xfffaf0e6},
    {"magenta", 0xffff00ff}, {"maroon", 0xff800000}, {"mediumaquamarine", 0xff66cdaa}, {"mediumblue", 0xff0000cd},
    {"mediumorchid", 0xffba55d3}, {"mediumpurple", 0xff9370db}, {"mediumseagreen", 0xff3cb371},
    {"mediumslateblue", 0xff7b68ee}, {"mediumspringgreen", 0xff00fa9a}, {"mediumturquoise", 0xff48d1cc},
    {"mediumvioletred", 0xffc71585}, {"midnightblue", 0xff191970}, {"mintcream", 0xfff5fffa},
    {"mistyrose", 0xffffe4e1}, {"moccasin", 0xffffe4b5}, {"navajowhite", 0xffffdead}, {"navy", 0xff000080},
    {"oldlace", 0xfffdf5e6}, {"olive", 0xff808000}, {"olivedrab", 0xff6b8e23}, {"orange", 0xffffa500},
    {"orangered", 0xffff4500}, {"orchid", 0xffda70d6}, {"palegoldenrod", 0xffeee8aa}, {"palegreen", 0xff98fb98},
    {"paleturquoise", 0xffafeeee}, {"palevioletred", 0xffdb7093}, {"papayawhip", 0xffffefd5},
    {"peachpuff", 0xffffdab9}, {"peru", 0xffcd853f}, {"pink", 0xffffc0cb}, {"plum", 0xffdda0dd},
    {"powderblue", 0xffb0e0e6}, {"purple", 0xff800080}, {"red", 0xffff0000}, {"rosybrown", 0xffbc8f8f},
    {"royalblue", 0xff4169e1}, {"saddlebrown", 0xff8b4513}, {"salmon", 0xfffa8072}, {"sandybrown", 0xfff4a460},
    {"seagreen", 0xff2e8b57}, {"seashell", 0xfffff5ee}, {"sienna", 0xffa0522d}, {"silver", 0xffc0c0c0},
    {"skyblue", 0xff87ceeb}, {"slateblue", 0xff6a5acd}, {"slategray", 0xff708090}, {"snow", 0xfffffafa},
    {"springgreen", 0xff00ff7f}, {"steelblue", 0xff4682b4}, {"tan", 0xffd2b48c}, {"teal", 0xff008080},
    {"thistle", 0xffd8bfd8}, {"tomato", 0xffff6347}, {"turquoise", 0xff40e0d0}, {"violet", 0xffee82ee},
    {"wheat", 0xfff5deb3}, {"white", 0xffffffff}, {"whitesmoke", 0xfff5f5f5}, {"yellow", 0xffffff00},
    {"yellowgreen", 0xff9acd32}, {"transparent", 0x00000000}, {"grey", 0xff808080},
};

}  // namespace

uint64_t NewImageSerial() {
  static std::atomic<uint64_t> next{1};
  return next++;
}

IntRect IntRect::Intersect(const IntRect& o) const {
  return IntRect{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
}

IntRect IntRect::Union(const IntRect& o) const {
  if (o.empty()) return *this;
  if (empty()) return o;
  return IntRect{std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}

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

void SoftwareTarget::ClearRect(const IntRect& r, uint32_t color) {
  const IntRect c = r.Intersect(IntRect{0, 0, width_, height_});
  for (int y = c.y0; y < c.y1; ++y) {
    uint32_t* row = &pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    std::fill(row + c.x0, row + c.x1, color);
  }
}

void SoftwareTarget::SetClipPixels(const IntRect& r) {
  cx0_ = std::max(0, r.x0), cy0_ = std::max(0, r.y0);
  cx1_ = std::min(width_, r.x1), cy1_ = std::min(height_, r.y1);
}

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
  Touch(x0, y0, x1, y1);
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = &pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    for (int x = x0; x < x1; ++x) Blend(row[x], r, g, b, a);
  }
}

void SoftwareTarget::DrawPixels(const uint32_t* src, int w, int h, int x, int y) {
  if (!src) return;
  int x0 = std::max(cx0_, x), x1 = std::min(cx1_, x + w);
  int y0 = std::max(cy0_, y), y1 = std::min(cy1_, y + h);
  if (x0 >= x1) return;
  Touch(x0, y0, x1, y1);
  for (int row = y0; row < y1; ++row) {
    const uint32_t* from = src + static_cast<size_t>(row - y) * static_cast<size_t>(w) + (x0 - x);
    uint32_t* to = &pixels_[static_cast<size_t>(row) * static_cast<size_t>(width_) + static_cast<size_t>(x0)];
    for (int i = 0; i < x1 - x0; ++i) to[i] = from[i] | 0xff000000u;
  }
}

void SoftwareTarget::DrawImage(const Image& img, const Rect& s, const Rect& d, uint32_t color) {
  if (!img.valid() || d.x1 <= d.x0 || d.y1 <= d.y0 || A(color) == 0) return;
  int x0 = std::max(cx0_, FirstPixel(d.x0)), x1 = std::min(cx1_, FirstPixel(d.x1));
  int y0 = std::max(cy0_, FirstPixel(d.y0)), y1 = std::min(cy1_, FirstPixel(d.y1));
  if (x0 >= x1 || y0 >= y1) return;
  Touch(x0, y0, x1, y1);
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
  Touch(std::max(cx0_, x), std::max(cy0_, y), std::min(cx1_, x + gl.width), std::min(cy1_, y + gl.height));
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
