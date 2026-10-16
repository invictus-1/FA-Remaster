#include "maui/font.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/vfs.h"
#include "draw/draw.h"

// GDI on Windows; elsewhere (and on Windows with MOHO64UI_STB_FONTS, to compare the two) the font
// files are read and drawn here with stb_truetype (ui/third_party/stb).
#if defined(_WIN32) && !defined(MOHO64UI_STB_FONTS)
#define MOHO64UI_GDI_FONTS 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb/stb_truetype.h"
#endif

namespace moho::ui {
namespace {

// UTF-8 to the 16-bit characters the original works with (a character beyond 16 bits keeps its
// low 16 bits; a zero ends the string).
std::vector<wchar_t> Decode(const std::string& s) {
  std::vector<wchar_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = c;
    size_t n = 1;
    if ((c >> 5) == 6) cp = c & 0x1f, n = 2;
    else if ((c >> 4) == 14) cp = c & 0x0f, n = 3;
    else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += n;
    wchar_t w = static_cast<wchar_t>(cp & 0xffff);
    if (!w) break;
    out.push_back(w);
  }
  return out;
}

#ifdef MOHO64UI_GDI_FONTS

struct GdiFont {
  HFONT font = nullptr;
  FontMetrics metrics;
  int cellHeight = 0, pad = 0;  // tmHeight and tmAveCharWidth
  std::unordered_map<wchar_t, float> advances;
  std::unordered_map<wchar_t, GlyphMask> glyphs;
};

// One memory DC with a 128x128 32-bit bitmap, as the original measures its characters on.
HDC MeasureDC() {
  static HDC dc = nullptr;
  if (!dc) {
    dc = CreateCompatibleDC(nullptr);
    if (dc) SelectObject(dc, CreateBitmap(128, 128, 1, 32, nullptr));
  }
  return dc;
}

GdiFont& Font(const std::string& family, int size) {
  static std::map<std::pair<std::string, int>, GdiFont> cache;
  auto key = std::make_pair(family, size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  GdiFont f;
  HDC dc = MeasureDC();
  f.font = CreateFontA(-size, 0, 0, 0, 0, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, family.c_str());
  if (dc && f.font) {
    SelectObject(dc, f.font);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) {
      f.metrics.height = static_cast<float>(tm.tmHeight);
      f.metrics.ascent = static_cast<float>(tm.tmAscent);
      f.metrics.descent = static_cast<float>(tm.tmDescent);
      f.metrics.internalLeading = static_cast<float>(tm.tmInternalLeading);
      f.metrics.externalLeading = static_cast<float>(tm.tmExternalLeading);
      f.cellHeight = tm.tmHeight;
      f.pad = tm.tmAveCharWidth;
    }
  } else {
    Logf(LogLevel::Warning, "moho64ui: cannot create font %s %d", family.c_str(), size);
  }
  return cache.emplace(key, std::move(f)).first->second;
}

float CharAdvance(GdiFont& f, wchar_t ch) {
  auto it = f.advances.find(ch);
  if (it != f.advances.end()) return it->second;
  float adv = 0;
  HDC dc = MeasureDC();
  if (dc && f.font) {
    SelectObject(dc, f.font);
    RECT rc{0, 0, 0, 0};
    DrawTextW(dc, &ch, 1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    adv = static_cast<float>(rc.right - rc.left);
  }
  f.advances.emplace(ch, adv);
  return adv;
}

// A character's picture, made as the original makes it (FA exe 0x425940): drawn in white on black
// with DrawText into a cell that leaves the font's average character width free on both sides
// (for parts that reach beyond the advance), then read back; a pixel's brightness is its
// coverage. The cell is cut down to the part that has ink.
const GlyphMask& Glyph(GdiFont& f, wchar_t ch) {
  auto it = f.glyphs.find(ch);
  if (it != f.glyphs.end()) return it->second;
  GlyphMask mask;
  int cellW = static_cast<int>(CharAdvance(f, ch)) + 2 * f.pad, cellH = f.cellHeight;
  static HDC dc = nullptr;
  static HBITMAP dib = nullptr;
  static uint32_t* bits = nullptr;
  static int dibW = 0, dibH = 0;
  if (cellW > 0 && cellH > 0 && f.font) {
    if (!dc) dc = CreateCompatibleDC(nullptr);
    if (dc && (cellW > dibW || cellH > dibH)) {
      dibW = std::max(dibW, std::max(cellW, 256));
      dibH = std::max(dibH, std::max(cellH, 128));
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof bi.bmiHeader;
      bi.bmiHeader.biWidth = dibW;
      bi.bmiHeader.biHeight = -dibH;  // rows from the top
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      void* p = nullptr;
      HBITMAP fresh = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
      if (fresh) {
        SelectObject(dc, fresh);
        if (dib) DeleteObject(dib);
        dib = fresh;
        bits = static_cast<uint32_t*>(p);
      }
    }
    if (dc && bits) {
      SelectObject(dc, f.font);
      RECT cell{0, 0, cellW, cellH};
      FillRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      SetBkColor(dc, RGB(0, 0, 0));
      SetTextColor(dc, RGB(255, 255, 255));
      RECT rc{f.pad, 0, cellW, cellH};
      DrawTextW(dc, &ch, 1, &rc, DT_NOPREFIX | DT_NOCLIP | DT_SINGLELINE);
      GdiFlush();
      int x0 = cellW, x1 = 0, y0 = cellH, y1 = 0;
      for (int y = 0; y < cellH; ++y)
        for (int x = 0; x < cellW; ++x)
          if (bits[static_cast<size_t>(y) * static_cast<size_t>(dibW) + static_cast<size_t>(x)] & 0x00ffffffu) {
            x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
            y0 = std::min(y0, y), y1 = std::max(y1, y + 1);
          }
      if (x1 > x0) {
        mask.width = x1 - x0;
        mask.height = y1 - y0;
        mask.left = x0 - f.pad;
        mask.top = y0;
        mask.coverage.resize(static_cast<size_t>(mask.width) * static_cast<size_t>(mask.height));
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x)
            mask.coverage[static_cast<size_t>(y - y0) * static_cast<size_t>(mask.width) + static_cast<size_t>(x - x0)] =
                static_cast<uint8_t>((bits[static_cast<size_t>(y) * static_cast<size_t>(dibW) + static_cast<size_t>(x)] >> 8) & 0xff);
      }
    }
  }
  return f.glyphs.emplace(ch, std::move(mask)).first->second;
}

#else  // MOHO64UI_GDI_FONTS: the font files' own drawing, with stb_truetype

// The font files, each kept whole as long as the program runs (stb_truetype reads from it): the
// game's (/fonts/*.ttf, InitFonts), then those of the system that stand in for a family the game
// has not.
std::vector<std::unique_ptr<std::string>>& FontFiles() {
  static std::vector<std::unique_ptr<std::string>> files;
  return files;
}

struct FileFont {
  stbtt_fontinfo info{};
  bool ok = false;
  float scale = 0;  // font units to pixels: the em is `size` pixels high, as GDI's CreateFont(-size)
  FontMetrics metrics;
  std::unordered_map<wchar_t, float> advances;
  std::unordered_map<wchar_t, GlyphMask> glyphs;
};

int U16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
int S16(const unsigned char* p) { return static_cast<int16_t>(static_cast<uint16_t>((p[0] << 8) | p[1])); }

// A face of the files by name, in GDI's order: one whose full name is the name ("Arial Bold", or
// the same in another language: "Arial Gras"), else the regular face of the family of that name,
// else any face of it. Null when no file has it.
const unsigned char* FindFace(const std::string& name, int* offset) {
  for (int flags : {0, STBTT_MACSTYLE_NONE, STBTT_MACSTYLE_BOLD, STBTT_MACSTYLE_ITALIC, STBTT_MACSTYLE_BOLD | STBTT_MACSTYLE_ITALIC}) {
    for (const auto& file : FontFiles()) {
      const auto* data = reinterpret_cast<const unsigned char*>(file->data());
      const int at = stbtt_FindMatchingFont(data, name.c_str(), flags);
      if (at >= 0) {
        *offset = at;
        return data;
      }
    }
  }
  return nullptr;
}

// The system's font file for a family the game's files do not have: on Linux the one fontconfig
// picks (it finds a stand-in: Liberation Sans for Arial); on a Mac the font folders by name.
std::string SystemFontFile(const std::string& family) {
  for (char c : family)  // (handed to a shell below: plain names only)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_') return std::string();
#if defined(__linux__)
  std::string out;
  const std::string command = "fc-match -f '%{file}' '" + family + "' 2>/dev/null";
  if (std::FILE* p = popen(command.c_str(), "r")) {
    char buf[1024];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
  }
  return out;
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  for (const std::string& folder : {std::string("/System/Library/Fonts/Supplemental/"), std::string("/Library/Fonts/"),
                                    std::string("/System/Library/Fonts/"), std::string(home ? home : "") + "/Library/Fonts/"})
    for (const char* ext : {".ttf", ".otf", ".ttc"}) {
      std::error_code ec;
      if (std::filesystem::exists(folder + family + ext, ec)) return folder + family + ext;
    }
  return std::string();
#else
  return std::string();
#endif
}

const unsigned char* AddSystemFont(const std::string& path, int* offset) {
  std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  auto data = std::make_unique<std::string>();
  char buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data->append(buf, n);
  std::fclose(f);
  const auto* bytes = reinterpret_cast<const unsigned char*>(data->data());
  const int at = stbtt_GetFontOffsetForIndex(bytes, 0);
  if (at < 0) return nullptr;
  Logf(LogLevel::Info, "moho64ui: system font %s", path.c_str());
  FontFiles().push_back(std::move(data));
  *offset = at;
  return bytes;
}

FileFont& Font(const std::string& family, int size) {
  static std::map<std::pair<std::string, int>, FileFont> cache;
  auto key = std::make_pair(family, size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  FileFont f;
  int offset = 0;
  const unsigned char* data = FindFace(family, &offset);
  if (!data) data = AddSystemFont(SystemFontFile(family), &offset);
  if (!data) data = FindFace("Arial", &offset);  // (the game's own, as a last stand-in)
  if (!data && !FontFiles().empty()) {
    data = reinterpret_cast<const unsigned char*>(FontFiles().front()->data());
    offset = stbtt_GetFontOffsetForIndex(data, 0);
  }
  if (data && offset >= 0 && stbtt_InitFont(&f.info, data, offset)) {
    f.ok = true;
    f.scale = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(size));
    // GDI's text metrics of a TrueType font: ascent and descent from the OS/2 table's Windows
    // values (the hhea table's without one), the external leading what the hhea line gap adds
    // beyond them.
    const unsigned char* d = f.info.data;
    const int hhea = f.info.hhea, os2 = static_cast<int>(stbtt__find_table(f.info.data, static_cast<stbtt_uint32>(f.info.fontstart), "OS/2"));
    const int hAscent = S16(d + hhea + 4), hDescent = S16(d + hhea + 6), lineGap = S16(d + hhea + 8);
    const int winAscent = os2 ? U16(d + os2 + 74) : hAscent, winDescent = os2 ? U16(d + os2 + 76) : -hDescent;
    f.metrics.ascent = std::floor(static_cast<float>(winAscent) * f.scale + 0.5f);
    f.metrics.descent = std::floor(static_cast<float>(winDescent) * f.scale + 0.5f);
    f.metrics.height = f.metrics.ascent + f.metrics.descent;
    f.metrics.internalLeading = f.metrics.height - static_cast<float>(size);
    const int extra = lineGap - ((winAscent + winDescent) - (hAscent - hDescent));
    f.metrics.externalLeading = extra > 0 ? std::floor(static_cast<float>(extra) * f.scale + 0.5f) : 0.0f;
  } else {
    Logf(LogLevel::Warning, "moho64ui: cannot create font %s %d", family.c_str(), size);
  }
  return cache.emplace(key, std::move(f)).first->second;
}

float CharAdvance(FileFont& f, wchar_t ch) {
  auto it = f.advances.find(ch);
  if (it != f.advances.end()) return it->second;
  float adv = 0;
  if (f.ok) {
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(ch), &advance, &bearing);
    adv = std::floor(static_cast<float>(advance) * f.scale + 0.5f);
  }
  f.advances.emplace(ch, adv);
  return adv;
}

// A character's picture: its coverage at the font's scale, placed as the GDI glyphs are (left from
// the pen position, top from the line's top).
const GlyphMask& Glyph(FileFont& f, wchar_t ch) {
  auto it = f.glyphs.find(ch);
  if (it != f.glyphs.end()) return it->second;
  GlyphMask mask;
  if (f.ok) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&f.info, static_cast<int>(ch), f.scale, f.scale, &x0, &y0, &x1, &y1);
    if (x1 > x0 && y1 > y0) {
      mask.width = x1 - x0;
      mask.height = y1 - y0;
      mask.left = x0;
      mask.top = static_cast<int>(f.metrics.ascent) + y0;
      mask.coverage.resize(static_cast<size_t>(mask.width) * static_cast<size_t>(mask.height));
      stbtt_MakeCodepointBitmap(&f.info, mask.coverage.data(), mask.width, mask.height, mask.width, f.scale, f.scale,
                                static_cast<int>(ch));
    }
  }
  return f.glyphs.emplace(ch, std::move(mask)).first->second;
}

#endif  // MOHO64UI_GDI_FONTS

}  // namespace

void InitFonts(const Vfs* vfs) {
  static bool done = false;
  if (done) return;
  done = true;
  for (const auto& file : vfs->FindFiles("/fonts", "*.ttf")) {
    Logf(LogLevel::Info, "adding font file %s", file.c_str());
    auto data = vfs->ReadFile(file);
    if (!data) {
      Logf(LogLevel::Warning, "D3D_InitFonts: cannot read %s", file.c_str());
      continue;
    }
#ifdef MOHO64UI_GDI_FONTS
    DWORD count = 0;
    // Private to this process and gone when it ends: nothing is installed on the system.
    if (!AddFontMemResourceEx(data->data(), static_cast<DWORD>(data->size()), nullptr, &count))
      Logf(LogLevel::Warning, "D3D_InitFonts: Error loading font %s", file.c_str());
#else
    FontFiles().push_back(std::make_unique<std::string>(std::move(*data)));
#endif
  }
}

FontMetrics GetFontMetrics(const std::string& family, int size) { return Font(family, size).metrics; }

float StringAdvance(const std::string& family, int size, const std::string& text) {
  auto& f = Font(family, size);
  float sum = 0;
  for (wchar_t ch : Decode(text)) sum += CharAdvance(f, ch);
  return sum;
}

void DrawString(DrawTarget& target, const std::string& family, int size, const std::string& text, float x, float baseline,
                uint32_t color, float maxAdvance) {
  auto& f = Font(family, size);
  int lineTop = static_cast<int>(std::floor(baseline - f.metrics.ascent + 0.5f));
  float pen = 0;
  for (wchar_t ch : Decode(text)) {
    float adv = CharAdvance(f, ch);
    if (maxAdvance >= 0 && pen + adv > maxAdvance) break;
    const GlyphMask& g = Glyph(f, ch);
    if (g.width) target.DrawGlyph(g, static_cast<int>(std::floor(x + pen + 0.5f)) + g.left, lineTop + g.top, color);
    pen += adv;
  }
}


}  // namespace moho::ui
