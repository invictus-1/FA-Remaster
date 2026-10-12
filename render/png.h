// Writes a PNG (screenshots, test captures). The image data is stored uncompressed: simple, and the
// files are only used for comparing pictures.
#pragma once
#include <cstdint>
#include <string>

namespace render {

// `rgba`: width * height * 4 bytes, top row first. The alpha channel is dropped (written as RGB).
bool WritePng(const std::string& path, const uint8_t* rgba, int width, int height);

}  // namespace render
