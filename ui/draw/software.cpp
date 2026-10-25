#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "core/hostfs.h"
#include "core/log.h"
#include "draw/draw.h"
#include "zlib.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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

#if defined(__SSE2__)
// Blend and Mul for two pixels at a time, their channels (B, G, R, A) in 16-bit lanes: the same integers as one at a
// time. Mul16's (x + 1 + (x >> 8)) >> 8 is x / 255 for every x = a * b + 127 of bytes a, b (checked for all 65536
// pairs), and Blend's special cases come out of its general sum (a source alpha of 0 leaves the pixel, 255 replaces
// it), which never passes 255. This program's own, only about time: a picture fading over the whole window at 4K.
inline __m128i Mul16(__m128i a, __m128i b) {
  const __m128i x = _mm_add_epi16(_mm_mullo_epi16(a, b), _mm_set1_epi16(127));
  return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(x, _mm_set1_epi16(1)), _mm_srli_epi16(x, 8)), 8);
}
inline __m128i Blend16(__m128i dst, __m128i src) {
  const __m128i alphaLanes = _mm_set_epi16(-1, 0, 0, 0, -1, 0, 0, 0);
  const __m128i a = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src, 0xff), 0xff);  // each pixel's alpha in its four lanes
  // (the colours times the alpha, the alpha itself as it is: Blend's oa = a + Mul(A(dst), 255 - a))
  const __m128i f = _mm_or_si128(_mm_andnot_si128(alphaLanes, a), _mm_and_si128(alphaLanes, _mm_set1_epi16(255)));
  return _mm_add_epi16(Mul16(src, f), Mul16(dst, _mm_sub_epi16(_mm_set1_epi16(255), a)));
}
// Four source pixels (0xAARRGGBB) over four of `row`, each times `color`'s channels unless `plain`.
inline void Blend4(uint32_t* row, const uint32_t* src, __m128i color16, bool plain) {
  const __m128i z = _mm_setzero_si128();
  const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
  const __m128i t = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
  __m128i tl = _mm_unpacklo_epi8(t, z), th = _mm_unpackhi_epi8(t, z);
  if (!plain) tl = Mul16(tl, color16), th = Mul16(th, color16);
  const __m128i lo = Blend16(_mm_unpacklo_epi8(d, z), tl), hi = Blend16(_mm_unpackhi_epi8(d, z), th);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(row), _mm_packus_epi16(lo, hi));
}
inline __m128i Lanes16(uint32_t color) {  // a colour's channels in the lanes of two pixels
  return _mm_unpacklo_epi8(_mm_set1_epi32(static_cast<int>(color)), _mm_setzero_si128());
}
#endif

struct Name {
  const char* name;
  uint32_t color;
};
// The colour names scripts may give, case not counting: the original's table (FA exe 0xdff690, 141
// names; the engine's copy, src/script/color_names.inc).
const Name kNames[] = {
#include "script/color_names.inc"
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

// As the original's parser (0x4b2b90): a name first, then exactly 6 or 8 hex digits (6: opaque). Unlike
// the original, an unknown text raises no "Unknown color: %s" (SCR_DecodeColor 0x4b2d20): it gives
// white, and the log names it once.
uint32_t ParseColor(const std::string& text) {
  auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
  for (const auto& n : kNames)
    if (text.size() == std::strlen(n.name) && std::equal(text.begin(), text.end(), n.name, same)) return n.color;
  bool hex = (text.size() == 8 || text.size() == 6);
  for (char c : text) hex = hex && std::isxdigit(static_cast<unsigned char>(c));
  if (hex) {
    uint32_t v = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
    return text.size() == 6 ? 0xff000000u | v : v;
  }
  static std::mutex m;
  static std::set<std::string> told;
  std::lock_guard<std::mutex> lock(m);
  if (told.insert(text).second) Logf(LogLevel::Warning, "moho64ui: unknown colour \"%s\" (white)", text.c_str());
  return 0xffffffff;
}

uint32_t WithAlpha(uint32_t color, float alpha) {
  float a = static_cast<float>(A(color)) * std::max(0.0f, std::min(1.0f, alpha));
  return (color & 0x00ffffffu) | static_cast<uint32_t>(a + 0.5f) << 24;
}

SoftwareTarget::SoftwareTarget(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u) {
  px_ = pixels_.data();
  ClearClip();
}

SoftwareTarget::SoftwareTarget(ViewOf view) : width_(view.shared.width_), height_(view.shared.height_), px_(view.shared.px_) {
  ClearClip();
}

void SoftwareTarget::Clear(uint32_t color) { std::fill(px_, px_ + static_cast<size_t>(width_) * static_cast<size_t>(height_), color); }

void SoftwareTarget::ClearRect(const IntRect& r, uint32_t color) {
  const IntRect c = r.Intersect(IntRect{0, 0, width_, height_});
  for (int y = c.y0; y < c.y1; ++y) {
    uint32_t* row = &px_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
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
#if defined(__SSE2__)
  const __m128i src = Lanes16(color);  // (four at a time: the same bytes as Blend)
#endif
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = &px_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    int x = x0;
#if defined(__SSE2__)
    if (a == 255) {
      std::fill(row + x0, row + std::max(x0, x1), 0xff000000u | r << 16 | g << 8 | b);
      continue;
    }
    const __m128i z = _mm_setzero_si128();
    for (; x + 4 <= x1; x += 4) {
      const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x));
      const __m128i lo = Blend16(_mm_unpacklo_epi8(d, z), src), hi = Blend16(_mm_unpackhi_epi8(d, z), src);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(row + x), _mm_packus_epi16(lo, hi));
    }
#endif
    for (; x < x1; ++x) Blend(row[x], r, g, b, a);
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
    uint32_t* to = &px_[static_cast<size_t>(row) * static_cast<size_t>(width_) + static_cast<size_t>(x0)];
    for (int i = 0; i < x1 - x0; ++i) to[i] = from[i] | 0xff000000u;
  }
}

namespace {
// A stretched picture's filtered texel for one pixel: false where the four texels round it are all transparent (the
// pixel is left as it is).
struct Column {
  int xa, xb;  // the texels left and right of the sample point (exact: xa alone)
  float wx;
};
inline bool Filtered(const uint32_t* rowA, const uint32_t* rowB, const Column& c, float wy, uint32_t* out) {
  const int xa = c.xa, xb = c.xb;
  const float wx = c.wx;
  uint32_t t00 = rowA[xa], t10 = rowA[xb];
  uint32_t t01 = rowB[xa], t11 = rowB[xb];
  // Filter with alpha as weight, so the colour of transparent texels does not bleed in.
  float w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
  float a00 = w00 * static_cast<float>(A(t00)), a10 = w10 * static_cast<float>(A(t10));
  float a01 = w01 * static_cast<float>(A(t01)), a11 = w11 * static_cast<float>(A(t11));
  float fa = a00 + a10 + a01 + a11;
  if (fa <= 0.0f) return false;
#if defined(__SSE2__)
  // The colours four at a time (B, G, R, A in the lanes): the same products and sums in the same order as one at a
  // time, and the same division and truncation, so the same bytes (the build contracts no multiply-adds:
  // -ffp-contract=off; checked against the plain code on 50 million random cases). The alpha lane is not used.
  auto lanes = [](uint32_t t) {
    const __m128i z = _mm_setzero_si128();
    return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(t)), z), z));
  };
  __m128 v = _mm_mul_ps(_mm_set1_ps(a00), lanes(t00));
  v = _mm_add_ps(v, _mm_mul_ps(_mm_set1_ps(a10), lanes(t10)));
  v = _mm_add_ps(v, _mm_mul_ps(_mm_set1_ps(a01), lanes(t01)));
  v = _mm_add_ps(v, _mm_mul_ps(_mm_set1_ps(a11), lanes(t11)));
  __m128i i = _mm_cvttps_epi32(_mm_add_ps(_mm_div_ps(v, _mm_set1_ps(fa)), _mm_set1_ps(0.5f)));
  i = _mm_packus_epi16(_mm_packs_epi32(i, i), i);
  *out = static_cast<uint32_t>(fa + 0.5f) << 24 | (static_cast<uint32_t>(_mm_cvtsi128_si32(i)) & 0x00ffffffu);
  return true;
#else
  uint32_t ta = static_cast<uint32_t>(fa + 0.5f);
  uint32_t tr = static_cast<uint32_t>((a00 * static_cast<float>(R(t00)) + a10 * static_cast<float>(R(t10)) + a01 * static_cast<float>(R(t01)) + a11 * static_cast<float>(R(t11))) / fa + 0.5f);
  uint32_t tg = static_cast<uint32_t>((a00 * static_cast<float>(G(t00)) + a10 * static_cast<float>(G(t10)) + a01 * static_cast<float>(G(t01)) + a11 * static_cast<float>(G(t11))) / fa + 0.5f);
  uint32_t tb = static_cast<uint32_t>((a00 * static_cast<float>(B(t00)) + a10 * static_cast<float>(B(t10)) + a01 * static_cast<float>(B(t01)) + a11 * static_cast<float>(B(t11))) / fa + 0.5f);
  *out = ta << 24 | tr << 16 | tg << 8 | tb;  // (each at most 255: weighted means of bytes)
  return true;
#endif
}

// A big stretched picture drawn again unchanged, only its colour new (a fade's alpha) or what lies under it: its
// filtered texels are kept, made row by row as they are first needed, and the next frames only blend them (the same
// pixels as filtering each time: the filter depends neither on the colour nor on the target). Only big ones, and only
// once one has been drawn whole in two frames one after the other (NextDrawFrame): a film's picture is drawn whole
// once, then only in part where something over it changes; the oldest go beyond kStretchedBytes. A row
// is made by one thread: the bands of a repaint (draw/recorder.cpp) never share one. This program's own, about time.
struct StretchedKey {
  uint64_t serial;
  float s[4], d[4];
  int width, height;  // the target's
  bool operator<(const StretchedKey& o) const {
    if (serial != o.serial) return serial < o.serial;
    if (width != o.width) return width < o.width;
    if (height != o.height) return height < o.height;
    for (int i = 0; i < 4; ++i)
      if (s[i] != o.s[i]) return s[i] < o.s[i];
    for (int i = 0; i < 4; ++i)
      if (d[i] != o.d[i]) return d[i] < o.d[i];
    return false;
  }
};
struct Stretched {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // its pixels within the target
  std::unique_ptr<uint32_t[]> texels;   // the filtered texel (A, R, G, B) of each pixel, 0 where there is none (rows made only)
  size_t bytes = 0;
  std::vector<uint8_t> rowMade;
  uint64_t frame = 0;      // the last frame it was drawn in
  long long drawnArea = 0;  // the pixels drawn of it in that frame (the bands of a repaint each draw a part)
  int wholeFrames = 0;      // the frames one after the other it was last drawn whole in
  uint64_t lastWhole = 0;   // the last of them
  uint64_t lastUse = 0;
};
constexpr long long kStretchedArea = 512 * 1024;       // the least kept (pixels)
constexpr size_t kStretchedBytes = size_t{160} << 20;  // all kept, at most
std::mutex g_stretchedLock;
uint64_t g_stretchedUses = 0;
std::atomic<uint64_t> g_drawFrame{1};
std::map<StretchedKey, std::shared_ptr<Stretched>>& StretchedPictures() {
  static std::map<StretchedKey, std::shared_ptr<Stretched>> kept;
  return kept;
}
// The kept picture for this drawing (its pixels in the target: `pixels`; `drawing`: the part drawn now), or nullptr
// while it is not kept.
std::shared_ptr<Stretched> KeptStretched(const StretchedKey& key, const IntRect& pixels, long long drawing) {
  std::lock_guard<std::mutex> lock(g_stretchedLock);
  auto& kept = StretchedPictures();
  std::shared_ptr<Stretched> e = kept[key];
  if (!e) e = kept[key] = std::make_shared<Stretched>();
  e->lastUse = ++g_stretchedUses;
  const uint64_t now = g_drawFrame;
  if (e->frame != now) {  // (the frame it was last drawn in is over: was it drawn whole?)
    if (e->frame != 0) {
      const bool whole = e->drawnArea * 10 >= pixels.area() * 9;
      e->wholeFrames = !whole ? 0 : e->lastWhole + 1 == e->frame ? e->wholeFrames + 1 : 1;
      if (whole) e->lastWhole = e->frame;
    }
    e->frame = now, e->drawnArea = 0;
  }
  e->drawnArea += drawing;
  if (!e->texels && e->wholeFrames < 2) return nullptr;
  if (!e->texels) {
    e->x0 = pixels.x0, e->y0 = pixels.y0, e->x1 = pixels.x1, e->y1 = pixels.y1;
    e->texels.reset(new uint32_t[static_cast<size_t>(pixels.area())]);  // (not cleared: a row is made before it is read)
    e->bytes = static_cast<size_t>(pixels.area()) * sizeof(uint32_t);
    e->rowMade.assign(static_cast<size_t>(pixels.y1 - pixels.y0), 0);
    for (;;) {  // (the oldest let go while all kept are over the bound; one in use lives on with its user)
      size_t bytes = 0;
      auto oldest = kept.end();
      for (auto it = kept.begin(); it != kept.end(); ++it) {
        bytes += it->second->bytes;
        if (it->second != e && it->second->texels && (oldest == kept.end() || it->second->lastUse < oldest->second->lastUse))
          oldest = it;
      }
      if (bytes <= kStretchedBytes || oldest == kept.end()) break;
      kept.erase(oldest);
    }
  }
  if (kept.size() > 256)  // (those only counted, drawn once a while ago, go)
    for (auto it = kept.begin(); it != kept.end();)
      it = !it->second->texels && it->second->lastUse + 256 < g_stretchedUses ? kept.erase(it) : std::next(it);
  return e;
}
}  // namespace

void NextDrawFrame() { ++g_drawFrame; }

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
  // (What does not change along a row or down a column is worked out once: the same arithmetic as per pixel.)
  // The four texels around a sample point stay inside the source rectangle, so a texture's border ring does not
  // leak in. (A source given right to left or bottom to top is the picture mirrored: a turned bitmap,
  // maui/bitmap.cpp.)
  const int minX = std::max(0, static_cast<int>(std::floor(std::min(s.x0, s.x1))));
  const int maxX = std::min(lastX, static_cast<int>(std::ceil(std::max(s.x0, s.x1))) - 1);
  const int minY = std::max(0, static_cast<int>(std::floor(std::min(s.y0, s.y1))));
  const int maxY = std::min(lastY, static_cast<int>(std::ceil(std::max(s.y0, s.y1))) - 1);
  // A big stretched picture drawn before: its kept texels, made over all its pixels in the target (not the clip's).
  std::shared_ptr<Stretched> kept;
  if (!exact) {
    const IntRect all = IntRect{FirstPixel(d.x0), FirstPixel(d.y0), FirstPixel(d.x1), FirstPixel(d.y1)}.Intersect(IntRect{0, 0, width_, height_});
    if (all.area() >= kStretchedArea)
      kept = KeptStretched(StretchedKey{img.serial, {s.x0, s.y0, s.x1, s.y1}, {d.x0, d.y0, d.x1, d.y1}, width_, height_}, all,
                           static_cast<long long>(x1 - x0) * (y1 - y0));
  }
  const int colFrom = kept ? kept->x0 : x0, colTo = kept ? kept->x1 : x1;
  thread_local std::vector<Column> columns;
  columns.resize(static_cast<size_t>(colTo - colFrom));
  for (int x = colFrom; x < colTo; ++x) {
    Column& c = columns[static_cast<size_t>(x - colFrom)];
    float u = s.x0 + (static_cast<float>(x) + 0.5f - d.x0) * sx;
    if (exact) {
      c.xa = c.xb = std::max(0, std::min(lastX, static_cast<int>(std::floor(u)))), c.wx = 0;
    } else {
      float fu = u - 0.5f;
      int tx = static_cast<int>(std::floor(fu));
      c.wx = fu - static_cast<float>(tx);
      c.xa = std::max(minX, std::min(maxX, tx)), c.xb = std::max(minX, std::min(maxX, tx + 1));
    }
  }
  const uint32_t* p = img.pixels.data();
  const size_t w = static_cast<size_t>(img.width);
  const Column* col = columns.data() + (x0 - colFrom);  // (the clip's first column)
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = &px_[static_cast<size_t>(y) * static_cast<size_t>(width_)];
    float v = s.y0 + (static_cast<float>(y) + 0.5f - d.y0) * sy;  // in texels; a texel's centre is at +0.5
    int ya, yb;
    float wy = 0;
    if (exact) {
      ya = yb = std::max(0, std::min(lastY, static_cast<int>(std::floor(v))));
    } else {
      float fv = v - 0.5f;
      int ty = static_cast<int>(std::floor(fv));
      wy = fv - static_cast<float>(ty);
      ya = std::max(minY, std::min(maxY, ty)), yb = std::max(minY, std::min(maxY, ty + 1));
    }
    const uint32_t* rowA = p + static_cast<size_t>(ya) * w;
    const uint32_t* rowB = p + static_cast<size_t>(yb) * w;
    if (kept) {
      const int keptW = kept->x1 - kept->x0;
      uint32_t* texels = kept->texels.get() + static_cast<size_t>(y - kept->y0) * static_cast<size_t>(keptW);
      uint8_t& made = kept->rowMade[static_cast<size_t>(y - kept->y0)];
      if (!made) {
        for (int i = 0; i < keptW; ++i)
          if (!Filtered(rowA, rowB, columns[static_cast<size_t>(i)], wy, &texels[i])) texels[i] = 0;
        made = 1;
      }
      int x = x0;
#if defined(__SSE2__)
      const __m128i color16 = Lanes16(color);  // (four at a time: the same bytes as below)
      for (; x + 4 <= x1; x += 4) Blend4(row + x, texels + (x - kept->x0), color16, plain);
#endif
      for (; x < x1; ++x) {
        const uint32_t t = texels[x - kept->x0];  // (0 where the filter found nothing: blends as nothing)
        uint32_t ta = A(t), tr = R(t), tg = G(t), tb = B(t);
        if (!plain) ta = Mul(ta, ca), tr = Mul(tr, cr), tg = Mul(tg, cg), tb = Mul(tb, cb);
        Blend(row[x], tr, tg, tb, ta);
      }
      continue;
    }
    for (int x = x0; x < x1; ++x) {
      uint32_t t;
      if (exact) t = rowA[col[x - x0].xa];
      else if (!Filtered(rowA, rowB, col[x - x0], wy, &t)) continue;
      uint32_t ta = A(t), tr = R(t), tg = G(t), tb = B(t);
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
    uint32_t* row = &px_[static_cast<size_t>(py) * static_cast<size_t>(width_)];
    const uint8_t* cov = &gl.coverage[static_cast<size_t>(gy) * static_cast<size_t>(gl.width)];
    for (int gx = 0; gx < gl.width; ++gx) {
      int px = x + gx;
      if (px < cx0_ || px >= cx1_ || !cov[gx]) continue;
      Blend(row[px], r, g, b, Mul(cov[gx], ca));
    }
  }
}

// A PNG with the pixels stored uncompressed (deflate "stored" blocks): simple, and every viewer reads it.
bool SoftwareTarget::SavePng(const std::string& hostPath) const { return SavePngPixels(hostPath, pixels_.data(), width_, height_); }

bool SavePngPixels(const std::string& hostPath, const uint32_t* pixels, int width, int height) {
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
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
  for (int y = 0; y < height; ++y) {
    raw += '\0';
    for (int x = 0; x < width; ++x) {
      uint32_t c = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
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
  be32(ihdr, static_cast<uint32_t>(width));
  be32(ihdr, static_cast<uint32_t>(height));
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bits, RGB
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", "");
  return hostfs::WriteFile(hostPath, out);
}

}  // namespace moho::ui
