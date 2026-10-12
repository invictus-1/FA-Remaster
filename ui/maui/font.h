// Fonts: the measures a text control gives its scripts (FontAscent, FontDescent,
// FontExternalLeading, TextAdvance) and drawing a string.
//
// The original (FA exe 0x425290, 0x425500, 0x425940, 0x426610) makes a Windows GDI font for a
// family name and a size: CreateFont with height = -size (the em height in pixels), default
// weight and character set, antialiased quality. Ascent, descent and the leadings are GDI's text
// metrics of that font. A character's advance is the width DrawText computes for that one
// character (DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE); a string's advance is the sum over its
// characters, without kerning. A character's picture is drawn with DrawText too, white on black,
// and read back. The game's own fonts (/fonts/*.ttf) are added to the process at start (0x4305f0).
//
// On Windows we make the same calls, so measures and glyphs are the original's. On other hosts
// there is no GDI: the measures there are rough proportions of the size and no text is drawn (a
// placeholder until the font files are read directly).
#pragma once
#include <cstdint>
#include <string>

namespace moho {
class Vfs;
}

namespace moho::ui {

class DrawTarget;

struct FontMetrics {
  float height = 0;  // GDI's tmHeight: ascent + descent
  float ascent = 0, descent = 0, internalLeading = 0, externalLeading = 0;
};

// Add the game's font files to the process (once). Logs "adding font file ..." per file.
void InitFonts(const Vfs* vfs);

FontMetrics GetFontMetrics(const std::string& family, int size);
// The width of a UTF-8 string in pixels.
float StringAdvance(const std::string& family, int size, const std::string& text);
// Draw a UTF-8 string with its pen starting at x on the given baseline. With maxAdvance >= 0 only
// the characters that fit in that width are drawn.
void DrawString(DrawTarget& target, const std::string& family, int size, const std::string& text, float x, float baseline,
                uint32_t color, float maxAdvance = -1.0f);

}  // namespace moho::ui
