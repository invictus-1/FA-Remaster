// Writes a PNG (screenshots, test captures). The image data is stored uncompressed: simple, and the
// files are only used for comparing pictures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace render {

// `rgba`: width * height * 4 bytes, top row first. The alpha channel is dropped (written as RGB).
bool WritePng(const std::string& path, const uint8_t* rgba, int width, int height);

// Reads a PNG of 16-bit grey values, not interlaced (a height picture: a map's surround, surround.h), from the file's
// bytes: `out` width * height values, top row first. False and why for anything else.
bool ReadPngGrey16(const std::string& file, int* width, int* height, std::vector<uint16_t>* out, std::string* why);

}  // namespace render
