// Font measures for layout: the numbers a Text control gives its scripts (FontAscent,
// FontDescent, FontExternalLeading, TextAdvance).
//
// PLACEHOLDER: these are rough proportions of the point size, not read from the font files yet.
// Layout that hangs on text (centred labels, wrapped text) is therefore only roughly where the
// original puts it. Reading the real fonts (the game's /fonts/*.ttf and the system's Arial) is the
// next piece of work on text.
#pragma once
#include <string>

namespace moho::ui {

struct FontMetrics {
  float ascent = 0, descent = 0, externalLeading = 0;
};

FontMetrics GetFontMetrics(const std::string& family, int pointSize);
// The width of a UTF-8 string in pixels.
float StringAdvance(const std::string& family, int pointSize, const std::string& text);

}  // namespace moho::ui
