#include "draw/recorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace moho::ui {
namespace {

// The whole pixels whose centres lie in [a, b) (as draw/software.cpp counts them).
inline int FirstPixel(float a) { return static_cast<int>(std::ceil(a - 0.5f)); }

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}
inline uint64_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}
uint64_t HashOf(const DrawCommand& c) {
  uint64_t h = Mix(0, static_cast<uint64_t>(c.kind));
  for (int v : {c.clip.x0, c.clip.y0, c.clip.x1, c.clip.y1, c.x, c.y, c.w, c.h}) h = Mix(h, static_cast<uint32_t>(v));
  for (float v : {c.src.x0, c.src.y0, c.src.x1, c.src.y1, c.dst.x0, c.dst.y0, c.dst.x1, c.dst.y1}) h = Mix(h, Bits(v));
  h = Mix(h, c.color);
  h = Mix(h, c.image ? c.image->serial : 0);
  h = Mix(h, reinterpret_cast<uintptr_t>(c.glyph));
  return h;
}

IntRect PixelsOf(const Rect& r) { return IntRect{FirstPixel(r.x0), FirstPixel(r.y0), FirstPixel(r.x1), FirstPixel(r.y1)}; }

constexpr int kTile = 32;  // the dirty parts are counted in tiles of this many pixels square

}  // namespace

// --- RecordingTarget ---

void RecordingTarget::Begin(int width, int height) {
  width_ = width, height_ = height;
  clip_ = IntRect{0, 0, width, height};
  commands_.clear();
}

void RecordingTarget::SetClip(const Rect& r) { clip_ = PixelsOf(r).Intersect(IntRect{0, 0, width_, height_}); }

void RecordingTarget::Add(DrawCommand& c) {
  c.clip = clip_;
  if (c.bounds.empty()) return;  // (it would draw nothing)
  c.hash = HashOf(c);
  commands_.push_back(c);
}

void RecordingTarget::FillRect(const Rect& dst, uint32_t color) {
  if ((color >> 24) == 0) return;
  DrawCommand c;
  c.kind = DrawCommand::Kind::Fill;
  c.dst = dst, c.color = color;
  c.bounds = PixelsOf(dst).Intersect(clip_);
  Add(c);
}

void RecordingTarget::DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) {
  if (!image.valid() || dst.x1 <= dst.x0 || dst.y1 <= dst.y0 || (color >> 24) == 0) return;
  DrawCommand c;
  c.kind = DrawCommand::Kind::Image;
  c.image = &image, c.src = src, c.dst = dst, c.color = color;
  c.bounds = PixelsOf(dst).Intersect(clip_);
  Add(c);
}

void RecordingTarget::DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) {
  if (!image.valid() || src.x1 - src.x0 <= 0 || src.y1 - src.y0 <= 0) return;
  DrawCommand c;
  c.kind = DrawCommand::Kind::Tiled;
  c.image = &image, c.src = src, c.dst = dst, c.color = color;
  c.bounds = PixelsOf(dst).Intersect(clip_);
  Add(c);
}

void RecordingTarget::DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) {
  if ((color >> 24) == 0) return;
  DrawCommand c;
  c.kind = DrawCommand::Kind::Glyph;
  c.glyph = &glyph, c.x = x, c.y = y, c.color = color;
  c.bounds = IntRect{x, y, x + glyph.width, y + glyph.height}.Intersect(clip_);
  Add(c);
}

void RecordingTarget::DrawPixels(const uint32_t* pixels, int width, int height, int x, int y) {
  if (!pixels) return;
  DrawCommand c;
  c.kind = DrawCommand::Kind::Pixels;
  c.pixels = pixels, c.x = x, c.y = y, c.w = width, c.h = height;
  c.bounds = IntRect{x, y, x + width, y + height}.Intersect(clip_);
  Add(c);
}

void Replay(const DrawCommand& c, SoftwareTarget& target, const IntRect& region) {
  const IntRect r = c.clip.Intersect(region);
  if (r.empty() || c.bounds.Intersect(r).empty()) return;
  target.SetClipPixels(r);
  switch (c.kind) {
    case DrawCommand::Kind::Fill: target.FillRect(c.dst, c.color); break;
    case DrawCommand::Kind::Image: target.DrawImage(*c.image, c.src, c.dst, c.color); break;
    case DrawCommand::Kind::Tiled: target.DrawImageTiled(*c.image, c.src, c.dst, c.color); break;
    case DrawCommand::Kind::Glyph: target.DrawGlyph(*c.glyph, c.x, c.y, c.color); break;
    case DrawCommand::Kind::Pixels: target.DrawPixels(c.pixels, c.w, c.h, c.x, c.y); break;
  }
}

// --- IncrementalPainter ---

IntRect IncrementalPainter::Paint(std::vector<DrawCommand>& now, SoftwareTarget& target, uint32_t clearColor,
                                  const IntRect& alsoDirty) {
  const int W = target.Width(), H = target.Height();
  const IntRect whole{0, 0, W, H};
  // Each command's key: its own hash with the one before it (a change of order counts too).
  std::vector<uint64_t> keys(now.size());
  for (size_t i = 0; i < now.size(); ++i) keys[i] = Mix(now[i].hash, i ? now[i - 1].hash : 0);

  const int tilesX = (W + kTile - 1) / kTile, tilesY = (H + kTile - 1) / kTile;
  bool full = !valid_ || W != width_ || H != height_ || clearColor != clear_;
  std::vector<IntRect> regions;
  if (!full) {
    tiles_.assign(static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY), 0);
    long long marked = 0;
    auto mark = [&](const IntRect& r0) {
      const IntRect r = r0.Intersect(whole);
      if (r.empty()) return;
      for (int ty = r.y0 / kTile; ty <= (r.y1 - 1) / kTile; ++ty)
        for (int tx = r.x0 / kTile; tx <= (r.x1 - 1) / kTile; ++tx) {
          uint8_t& t = tiles_[static_cast<size_t>(ty) * static_cast<size_t>(tilesX) + static_cast<size_t>(tx)];
          if (!t) t = 1, ++marked;
        }
    };
    std::unordered_map<uint64_t, int> left;  // the last frame's commands not yet matched
    left.reserve(lastKeys_.size() * 2);
    for (size_t j = 0; j < last_.size(); ++j)
      if (last_[j].kind != DrawCommand::Kind::Pixels) ++left[lastKeys_[j]];
    for (size_t i = 0; i < now.size(); ++i) {
      if (now[i].kind != DrawCommand::Kind::Pixels) {
        auto it = left.find(keys[i]);
        if (it != left.end() && it->second > 0) {
          --it->second;
          continue;
        }
      }
      mark(now[i].bounds);  // new, moved, changed, or a picture that changes every frame
    }
    for (size_t j = 0; j < last_.size(); ++j) {  // gone: where they were must be painted again
      if (last_[j].kind == DrawCommand::Kind::Pixels) {
        mark(last_[j].bounds);
        continue;
      }
      auto it = left.find(lastKeys_[j]);
      if (it != left.end() && it->second > 0) {
        --it->second;
        mark(last_[j].bounds);
      }
    }
    mark(alsoDirty);
    if (marked * 2 > static_cast<long long>(tiles_.size())) {
      full = true;  // (most of it: at once)
    } else {
      // The marked tiles as rectangles: runs along each row, then runs of the same span stacked.
      for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX;) {
          if (!tiles_[static_cast<size_t>(ty) * static_cast<size_t>(tilesX) + static_cast<size_t>(tx)]) {
            ++tx;
            continue;
          }
          int end = tx;
          while (end < tilesX && tiles_[static_cast<size_t>(ty) * static_cast<size_t>(tilesX) + static_cast<size_t>(end)]) ++end;
          regions.push_back(IntRect{tx * kTile, ty * kTile, std::min(W, end * kTile), std::min(H, (ty + 1) * kTile)});
          tx = end;
        }
      std::stable_sort(regions.begin(), regions.end(), [](const IntRect& a, const IntRect& b) {
        return a.x0 != b.x0 ? a.x0 < b.x0 : a.x1 != b.x1 ? a.x1 < b.x1 : a.y0 < b.y0;
      });
      std::vector<IntRect> merged;
      for (const IntRect& r : regions) {
        if (!merged.empty() && merged.back().x0 == r.x0 && merged.back().x1 == r.x1 && merged.back().y1 == r.y0)
          merged.back().y1 = r.y1;
        else
          merged.push_back(r);
      }
      regions.swap(merged);
    }
  }
  if (full) regions.assign(1, whole);

  IntRect changed;
  for (const IntRect& r : regions) {
    target.ClearRect(r, clearColor);
    for (const DrawCommand& c : now) Replay(c, target, r);
    changed = changed.Union(r);
    pixelsPainted += r.area();
  }
  target.ClearClip();
  ++frames;
  if (full) ++fullFrames;
  last_.swap(now);
  lastKeys_.swap(keys);
  width_ = W, height_ = H, clear_ = clearColor;
  valid_ = true;
  return changed;
}

long long IncrementalPainter::Check(const SoftwareTarget& target, IntRect* box) {
  const int W = target.Width(), H = target.Height();
  SoftwareTarget full(W, H);
  full.Clear(clear_);
  for (const DrawCommand& c : last_) Replay(c, full, IntRect{0, 0, W, H});
  full.ClearClip();
  long long differ = 0;
  IntRect b;
  const std::vector<uint32_t>&a = target.pixels(), &f = full.pixels();
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
      if (a[i] != f[i]) ++differ, b = b.Union(IntRect{x, y, x + 1, y + 1});
    }
  if (box) *box = b;
  return differ;
}

}  // namespace moho::ui
