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

// A number no image had before: an image's `serial` names its content, so a drawing that is
// compared with the last frame's (draw/recorder.h) sees new content even under the same address.
uint64_t NewImageSerial();

struct Image {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;  // 0xAARRGGBB, row by row from the top
  uint64_t serial = NewImageSerial();  // (a copy keeps it; new pixels in place need a new one)
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

// Whole pixels [x0, x1) x [y0, y1).
struct IntRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
  long long area() const { return empty() ? 0 : static_cast<long long>(x1 - x0) * (y1 - y0); }
  IntRect Intersect(const IntRect& o) const;
  IntRect Union(const IntRect& o) const;  // the box round both (an empty one adds nothing)
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
  // A block of width * height opaque pixels (0xAARRGGBB, rows from the top; alpha is not looked
  // at), its top-left pixel at (x, y): the world's picture under the controls.
  virtual void DrawPixels(const uint32_t* pixels, int width, int height, int x, int y) = 0;
  // Nothing has been drawn into the frame yet (a control drawing now lies under nothing of the interface). Only a
  // target that knows says so (the recorder, draw/recorder.h); the others: false.
  virtual bool Blank() const { return false; }
  // What has been drawn into the frame so far is hidden by what is drawn now (an opaque picture over all of it, given
  // to the graphics card: maui/control.h OfferToCard): left out. Only a target that can say so (the recorder).
  virtual void DropBelow() {}
};

// The software backend: draws into its own pixel buffer.
class SoftwareTarget : public DrawTarget {
 public:
  SoftwareTarget(int width, int height);
  // A view of `shared`'s pixels with a clip and a touched box of its own (no pixels of its own: pixels() is empty).
  // Views drawing into rows apart may run on threads side by side (draw/recorder.cpp: a big repaint in bands).
  struct ViewOf {
    SoftwareTarget& shared;
  };
  explicit SoftwareTarget(ViewOf view);
  SoftwareTarget(const SoftwareTarget&) = delete;
  SoftwareTarget& operator=(const SoftwareTarget&) = delete;
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  void SetClip(const Rect& r) override;
  void ClearClip() override;
  void FillRect(const Rect& dst, uint32_t color) override;
  void DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) override;
  void DrawPixels(const uint32_t* pixels, int width, int height, int x, int y) override;

  void Clear(uint32_t color);
  // These pixels set to `color` (no blending), within the target.
  void ClearRect(const IntRect& r, uint32_t color);
  // The clip in whole pixels (within the target).
  void SetClipPixels(const IntRect& r);
  const std::vector<uint32_t>& pixels() const { return pixels_; }  // 0xAARRGGBB rows from the top
  bool SavePng(const std::string& hostPath) const;
  // The box round every pixel drawn since ResetTouched (Clear and ClearRect do not count).
  void ResetTouched() { touched_ = IntRect{}; }
  const IntRect& Touched() const { return touched_; }

 private:
  void Touch(int x0, int y0, int x1, int y1) {
    if (x0 < x1 && y0 < y1) touched_ = touched_.Union(IntRect{x0, y0, x1, y1});
  }
  int width_, height_;
  int cx0_, cy0_, cx1_, cy1_;  // clip, in whole pixels
  std::vector<uint32_t> pixels_;
  uint32_t* px_;  // the pixels drawn into: pixels_'s, or the shared target's for a view
  IntRect touched_;
};

// A new frame of drawing begins (IncrementalPainter::Paint): a big stretched picture drawn again in a later frame
// keeps its filtered texels (draw/software.cpp).
void NextDrawFrame();

// Pixels (0xAARRGGBB rows from the top; alpha not looked at) saved as a PNG, RGB, the pixels stored uncompressed
// (the zlib here is its inflate side only): what SoftwareTarget::SavePng writes, for a block of any size.
bool SavePngPixels(const std::string& hostPath, const uint32_t* pixels, int width, int height);

// Decode a texture file's top mip level (.dds: DXT1/3/5 and uncompressed formats; .png, .bmp, .tga;
// known by content, not by name). An image that cannot be read is empty. Images are cached per file.
const Image& LoadTexture(const Vfs* vfs, const std::string& vpath);
// Decode a texture file's bytes (the formats LoadTexture reads). False when they cannot be read.
bool DecodeImage(const std::string& data, Image& out);
// Texture files decoded ahead on the calling thread (ours, only about time: the skirmish lobby's faction painting,
// 1900x1080, took 57-72 ms of its first frame). LoadTexture takes such an image when it first asks for its file (it
// waits for one being decoded at that moment; one not begun yet it decodes itself, and it is no longer done here).
// Decoded and never asked for, an image is kept until the program ends.
void DecodeTexturesAhead(const Vfs* vfs, const std::vector<std::string>& vpaths);

// A colour as scripts give it: "AARRGGBB" or "RRGGBB" in hex (any case), or a colour name.
// What cannot be read is opaque white.
uint32_t ParseColor(const std::string& text);
// The colour with its alpha multiplied by a control's alpha (0..1).
uint32_t WithAlpha(uint32_t color, float alpha);

}  // namespace moho::ui
