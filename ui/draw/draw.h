// The interface's drawing: a small 2D target (textured rectangles, filled rectangles, glyphs, a
// clip rectangle) that the controls draw themselves on. The first backend is a software one that
// fills a pixel buffer (shown in a window, or saved as a picture); a GPU backend of the renderer
// track can take its place behind the same interface.
//
// Colours are 0xAARRGGBB with straight (not premultiplied) alpha. Coordinates are pixels, x to
// the right and y down; a rectangle covers the pixels whose centres lie inside it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace moho {
class Vfs;
}

namespace moho::ui {

struct Image {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;  // 0xAARRGGBB, row by row from the top
  bool valid() const { return width > 0 && height > 0; }
};

// A glyph's coverage: one byte per pixel (0 = empty, 255 = full).
struct GlyphMask {
  int width = 0, height = 0;
  int left = 0;  // where the mask's left edge lies relative to the pen position
  int top = 0;   // where its top edge lies relative to the top of the line (baseline - ascent)
  std::vector<uint8_t> coverage;
};

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class DrawTarget {
 public:
  virtual ~DrawTarget() = default;
  virtual int Width() const = 0;
  virtual int Height() const = 0;
  // Drawing is limited to this rectangle until it is changed (the whole target at first).
  virtual void SetClip(const Rect& r) = 0;
  virtual void ClearClip() = 0;
  virtual void FillRect(const Rect& dst, uint32_t color) = 0;
  // The part `src` of an image (in its pixels) stretched over `dst`, multiplied by `color`.
  virtual void DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) = 0;
  // The part `src` of an image repeated over `dst` at its own size.
  virtual void DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) = 0;
  // A glyph in `color`, its mask's top-left pixel at (x, y).
  virtual void DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) = 0;
};

// The software backend: draws into its own pixel buffer.
class SoftwareTarget : public DrawTarget {
 public:
  SoftwareTarget(int width, int height);
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  void SetClip(const Rect& r) override;
  void ClearClip() override;
  void FillRect(const Rect& dst, uint32_t color) override;
  void DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) override;

  void Clear(uint32_t color);
  const std::vector<uint32_t>& pixels() const { return pixels_; }  // 0xAARRGGBB rows from the top
  bool SavePng(const std::string& hostPath) const;

 private:
  int width_, height_;
  int cx0_, cy0_, cx1_, cy1_;  // clip, in whole pixels
  std::vector<uint32_t> pixels_;
};

// Decode a texture file's top mip level (.dds: DXT1/3/5 and uncompressed formats). An image that
// cannot be read is empty. Images are cached per file.
const Image& LoadTexture(const Vfs* vfs, const std::string& vpath);

// A colour as scripts give it: "AARRGGBB" or "RRGGBB" in hex (any case), or a colour name.
// What cannot be read is opaque white.
uint32_t ParseColor(const std::string& text);
// The colour with its alpha multiplied by a control's alpha (0..1).
uint32_t WithAlpha(uint32_t color, float alpha);

}  // namespace moho::ui
