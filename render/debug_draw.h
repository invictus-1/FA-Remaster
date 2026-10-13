// Immediate drawing of lines and plain lit boxes: markers, placeholders, things under development.
// Add shapes during the frame, then Flush once in the scene's pass; the lists are emptied by it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class DebugDraw {
 public:
  explicit DebugDraw(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);

  // Colours are 0xAABBGGRR.
  void Line(Vec3 a, Vec3 b, uint32_t color);
  // A line of a width in pixels on the screen, the same from near and from far (a flat ribbon
  // turned to the eye; made at Flush, when the view is known).
  void WideLine(Vec3 a, Vec3 b, uint32_t color, float pixels);
  void Box(Vec3 center, Vec3 halfSize, uint32_t color);
  void Flush(const View& view);

 private:
  struct Vertex {
    Vec3 position;
    Vec3 normal;  // zero for lines: drawn unlit
    uint32_t color;
  };
  gfx::Device* device_;
  struct Wide {
    Vec3 a, b;
    uint32_t color;
    float pixels;
  };
  gfx::Pipeline solid_, lines_, wide_;
  gfx::Buffer solidBuffer_, lineBuffer_, wideBuffer_;
  std::vector<Vertex> solidVertices_, lineVertices_, wideVertices_;
  std::vector<Wide> wide_lines_;
};

}  // namespace render
