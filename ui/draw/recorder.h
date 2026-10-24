// The interface drawn only where it changed. A frame's drawing is first recorded (RecordingTarget:
// the DrawTarget calls as a list, with the clip each was made under and the pixels it may touch);
// the IncrementalPainter compares the list with the last frame's and paints into the kept picture
// only the parts whose drawing differs: there it clears and replays every command that reaches
// them, so those pixels come out as a full redraw would make them (the software target's pixels
// depend on a call's own rectangles, never on the clip; draw/software.cpp).
//
// What counts as the same drawing: the same kind of call with the same rectangles, colour, clip and
// content (an image by its `serial`, a glyph by its address: the glyph caches never free one), with
// the same call before it (so a change of order counts too). DrawPixels (the world's picture, the
// minimap, a movie) changes under the same address every frame: it always counts as changed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "draw/draw.h"

namespace moho::ui {

struct DrawCommand {
  enum class Kind : uint8_t { Fill, Image, Tiled, Glyph, Pixels };
  Kind kind = Kind::Fill;
  IntRect clip;    // the clip it was made under
  IntRect bounds;  // the pixels it may touch: its rectangle within the clip
  Rect src, dst;
  const Image* image = nullptr;
  const GlyphMask* glyph = nullptr;
  const uint32_t* pixels = nullptr;
  int x = 0, y = 0, w = 0, h = 0;  // a glyph's place; a block of pixels' place and size
  uint32_t color = 0;
  uint64_t hash = 0;
};

class RecordingTarget : public DrawTarget {
 public:
  // A new frame of this size: the list emptied, the clip the whole target.
  void Begin(int width, int height);
  int Width() const override { return width_; }
  int Height() const override { return height_; }
  void SetClip(const Rect& r) override;
  void ClearClip() override { clip_ = IntRect{0, 0, width_, height_}; }
  void FillRect(const Rect& dst, uint32_t color) override;
  void DrawImage(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawImageTiled(const Image& image, const Rect& src, const Rect& dst, uint32_t color) override;
  void DrawGlyph(const GlyphMask& glyph, int x, int y, uint32_t color) override;
  void DrawPixels(const uint32_t* pixels, int width, int height, int x, int y) override;
  std::vector<DrawCommand>& commands() { return commands_; }

 private:
  void Add(DrawCommand& c);
  int width_ = 0, height_ = 0;
  IntRect clip_;
  std::vector<DrawCommand> commands_;
};

// The command drawn into `target`, limited to `region` (and its own clip).
void Replay(const DrawCommand& c, SoftwareTarget& target, const IntRect& region);

class IncrementalPainter {
 public:
  // For the long-frame log (MOHO64UI_LONG_FRAMES): each Paint times its commands (on this thread's band) and keeps
  // the slowest, described in `slowest` ("" when none took timeSlowerThan ms).
  bool timeCommands = false;
  double timeSlowerThan = 0.5;
  std::string slowest;
  double slowestMs = 0;
  // Paint `now` into `target` where it differs from the list painted last (and over `alsoDirty`,
  // pixels someone else drew there); the rest of `target` is left as it is. The first time, after
  // Invalidate, and when the target's size or the clear colour is another: all of it. Returns the
  // part of the target that changed. `now` is kept (swapped with the old list, which the caller
  // may reuse).
  IntRect Paint(std::vector<DrawCommand>& now, SoftwareTarget& target, uint32_t clearColor, const IntRect& alsoDirty);
  void Invalidate() { valid_ = false; }
  // For tests (MOHO64UI_DIRTY_CHECK): the pixels of `target` that differ from a full redraw of the
  // list painted last, and the box round them. Only valid while that list's images live (the
  // frame it was painted in).
  long long Check(const SoftwareTarget& target, IntRect* box);
  // Frames painted, of them all at once, and the pixels repainted.
  long long frames = 0, fullFrames = 0, pixelsPainted = 0;

 private:
  std::vector<DrawCommand> last_;
  std::vector<uint64_t> lastKeys_;
  int width_ = 0, height_ = 0;
  uint32_t clear_ = 0;
  bool valid_ = false;
  std::vector<uint8_t> tiles_;  // scratch: the dirty tiles
};

}  // namespace moho::ui
