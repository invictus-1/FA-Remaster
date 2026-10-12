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
  void Box(Vec3 center, Vec3 halfSize, uint32_t color);
  void Flush(const View& view);

 private:
  struct Vertex {
    Vec3 position;
    Vec3 normal;  // zero for lines: drawn unlit
    uint32_t color;
  };
  gfx::Device* device_;
  gfx::Pipeline solid_, lines_;
  gfx::Buffer solidBuffer_, lineBuffer_;
  std::vector<Vertex> solidVertices_, lineVertices_;
};

}  // namespace render
